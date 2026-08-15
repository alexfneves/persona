# Context for: persona daemon — mic → STT (with endpointing) → AI agent, on top of audio.cpp

## TL;DR capability table

| Capability | In audio.cpp? | How |
|---|---|---|
| Speech-to-text (STT/ASR) | ✅ Yes — 11 ASR families | `qwen3_asr`, `nemotron_asr`, `voxtral_realtime`, `higgs_audio_stt`, `kroko_asr`, `parakeet_tdt`, `sense_asr` (streaming); `fun_asr_nano`, `citrinet_asr`, `hviske_asr`, `vibevoice_asr` (offline) |
| Real-time / streaming ASR | ✅ Yes | `--mode streaming`; C++ `IStreamingVoiceTaskSession` + `process_audio_chunk()` emits partial transcripts |
| Continuous dictation + endpointing | ⚠️ Partial | No turn-taking/endpoint API *per se*. Endpointing = watch VAD events + `finish_stream()`; Kroko ASR has opt-in endpoint segmentation (`docs/asr.md:93`) |
| VAD (voice activity) | ✅ Yes | `silero_vad` (bundled, offline + **streaming**), `marblenet_vad` (bundled, offline). Stream emits `VoiceActivityEvent::SpeechStart/SpeechEnd` |
| Microphone capture | ❌ **No** | audio.cpp has **no portaudio/alsa/pulse capture**. It accepts raw PCM (WAV, or stdin via `--audio -`, or your own `AudioChunkReader`). **We must capture mic ourselves** (portaudio/alsa/webrtc) and feed frames |
| TTS | ✅ Yes — many families | `qwen3_tts`, `chatterbox`, `voxcpm2`, `supertonic`, `omnivoice`, `dots_tts`, `pocket_tts`, `index_tts2`, etc. No piper/kokoro. Streaming out via `StreamEvent.audio_output` (chunked/pseudo-streaming) |
| Real-time audio playback | ❌ **No** device API | Audio is returned as float PCM buffers (`AudioBuffer`); **we must play it ourselves** (portaudio/alsa/pipewire) |
| HF model downloads | ✅ Yes | `tools/model_manager_v2.py` installs from `huggingface.co/<repo>/resolve/...` into `models/`; GGUF packages at `audio-cpp/audio.cpp-gguf` on HF |
| Model format | GGUF (primary), safetensors (default-loader) | GGUF 16-bit / Q8_0 / Q4_K / INT8 per family |
| Build system | CMake (C++17) | Static lib `engine_runtime` + per-family OBJECT libs baked in; backends CPU/CUDA/Vulkan/Metal/HIP |
| License | Apache-2.0 (2026 ShugoAI LLC) | Linkable for our purposes |

---

## Part 1: Local project (persona) — brief

- **Build**: Nix (flake) + devenv. `flake.nix` builds `persona` via `pkgs.stdenv.mkDerivation` compiling `src/persona.cpp` with `clang++ -O2 -std=c++17` → `$out/bin/persona`. `result` symlink exists.
- **Dev shell**: `devenv.lib.mkShell`, `languages.cplusplus.enable = true`. `.envrc` loads the flake (nix-direnv 3.1.0, `use flake . --no-pure-eval`). `.devenv/` holds devenv state (profile → nix store, run dir).
- **Source**: `src/persona.cpp` is a hello-world `main()` printing "Hello World". Nothing else exists.
- **Conventions**: C++17, clang, no tests, no linting config, no CMake (raw clang++ line in flake). Adding audio.cpp means adding a CMake-based dependency into a flake — note `docs/build/nixos.md` in audio.cpp is the Nix reference (there is a `flake.nix` in the audio.cpp repo too, and `external/ggml` is vendored).

---

## Part 2: audio.cpp deep dive

Scratch clone at `.scratch/audio.cpp` (HEAD `73b21f9e`, 2026-08-14; release tags exist: `release-0.6` = 2026-08-13, `release-0.5`). ~90KB `CMakeLists.txt`, 49 model families, 70+ variants.

### a. STT — engines, streaming, mic input

**Supported ASR families** (README.md table rows 75–104; docs/asr.md table):

| Family | Mode | Notes |
|---|---|---|
| `qwen3_asr` | offline + **streaming** | Qwen3-ASR-0.6B / 1.7B-hf; emits buffered transcript deltas (`docs/asr.md:47-54`) |
| `nemotron_asr` | offline + **streaming** | Nemotron 3.5 ASR RNNT 0.6B; 1s preferred chunks (`docs/asr.md:241-270`) |
| `voxtral_realtime` | offline + **streaming** | Voxtral-Mini-4B-Realtime-2602; Q8_0 default install, Q4_K variant (`docs/asr.md:387+`) |
| `higgs_audio_stt` | offline + **streaming** | Higgs Audio v3 STT; ~4s preferred chunks (`docs/asr.md:118-151`) |
| `kroko_asr` | offline + **streaming** | Zipformer2/RNN-T; **opt-in endpoint segmentation** (`docs/asr.md:93`) |
| `parakeet_tdt` | offline + streaming | buffered-streaming (`docs/community_models/models.md:26`) |
| `sense_asr` | offline + **streaming** | SenseVoice-Small SAN-M+CTC (`docs/asr.md:305-318`) |
| `fun_asr_nano` | offline | multilingual zh/en/ja (`docs/asr.md:36-45`) |
| `citrinet_asr`, `hviske_asr`, `vibevoice_asr` | offline | |

No native whisper.cpp family — GGUF is a container, not an adapter; existing whisper.cpp GGUFs are not automatically loadable (README.md:729-731, docs/gguf.md:5).

**Mic input: NOT built in.** The CLI accepts a WAV path, or raw PCM from stdin with `--mode streaming`:
```bash
audiocpp_cli --task asr --mode streaming --family qwen3_asr --model models/Qwen3-ASR-1.7B-hf --backend cuda --audio - --input-format s16le --input-rate 16000 --input-channels 1
```
(`--audio -` = stdin raw PCM; `--input-format`/`--input-rate`/`--input-channels` defaults s16le/16000/1 — docs/usage.md "Common Inputs And Outputs").

Under the hood: `app/streaming/pcm_source.h` — `make_stdin_pcm_stream(format, sample_format)` / `make_pcm_chunk_stream(std::istream&, ...)` produce an `AudioChunkStream` whose `AudioChunkReader` is `std::function<bool(int64_t max_samples, std::vector<float>& samples)>` (blocking pull). `app/streaming/streaming.cpp` `run_streaming_task()` loops `session.process_audio_chunk({sample_rate, channels, start_sample, samples})` and forwards events to a `StreamEventSink` (`std::function<void(const StreamEvent&)>`).

**C++ API for our daemon** (`include/engine/framework/runtime/session.h`):
```cpp
class IStreamingVoiceTaskSession : public virtual IVoiceTaskSession {
    virtual StreamingPolicy streaming_policy() const;      // preferred_audio_chunk_samples (e.g. 512)
    virtual void start_stream(const TaskRequest& request);
    virtual std::optional<StreamEvent> next_stream_event();       // pull
    virtual void set_stream_event_sink(StreamEventCallback sink); // push
    virtual TaskResult finish_stream();                            // finalize → final transcript
};
// StreamEvent carries partial_text (Transcript), is_final, audio_output, voice_activity...
```
So the daemon loop: our mic thread → `AudioChunkReader` → `run_streaming_task(session, request, sink, stream)` OR manual `start_stream`/`process_audio_chunk`/`finish_stream` loop.

### b. VAD / endpointing

**Yes — two bundled VAD models** (docs/speech_analysis.md):

- **`silero_vad`** — `assets/framework/models/silero_vad` (bundled, ships in repo, no download). Modes: **offline + streaming**. Streaming needs **512-sample chunks @ 16 kHz**.
  ```
  audiocpp_cli --task vad --family silero_vad --model assets/framework/models/silero_vad --backend cuda --mode streaming --audio <512-sample-16k-wav> --segments-out segments.json
  ```
  Options (per docs/speech_analysis.md table): `--request-option threshold=<float>` (default 0.5), `neg_threshold` (default threshold−0.15, min 0.01), `min_speech_duration_ms` (250), `min_silence_duration_ms` (100), `speech_pad_ms` (30), `max_speech_duration_s`. Offline VAD chunk planning: `--vad-chunks-out`, `--vad-chunk-max-seconds 45`, `--vad-chunk-merge-gap-seconds 0.5`, `--vad-chunk-padding-seconds 0.25`.
- **`marblenet_vad`** — `assets/framework/models/marblenet_vad`, offline only, single `threshold` option.
- Diarization: `sortformer_diar` (offline).

**Endpointing in C++**: streaming VAD emits `VoiceActivityEvent` with `Kind::SpeechStart / SpeechEnd / SpeechSegment` (`include/engine/framework/runtime/session.h:181-192`):
```cpp
struct VoiceActivityEvent {
    enum class Kind { SpeechStart, SpeechEnd, SpeechSegment };
    Kind kind; int64_t sample; float probability; std::optional<SpeechSegment> segment;
};
```
So "user stopped speaking" = `SpeechEnd` event from a silero_vad streaming session; then `finish_stream()` on the ASR session gives the final text. Alternatively Kroko ASR has built-in opt-in endpoint segmentation as a request option (docs/asr.md:93).

**Architecture implication**: run TWO streaming sessions in parallel on the same mic frames — silero_vad for endpointing, an ASR session for text. Both accept the same `AudioChunk` stream; you can fan out (one reader feeding two sessions) or just watch the same events (VAD events are also attached to StreamEvent.voice_activity if you run VAD through the same runner).

### c. TTS

**Yes — many families** (README.md Supported Models table): `qwen3_tts`, `chatterbox`, `voxcpm2`, `supertonic`, `omnivoice` (646+ langs), `dots_tts`, `pocket_tts` (100M, light), `index_tts2`, `irodori_tts` (ja), `moss_tts_nano` (100M), `higgs_audio_tts` (4B), `fish_audio`, `miotts`, `dramabox`, `vibevoice`, `glm_tts`, `confucius4_tts`, `neutts`, `vevo2`, `minimax_h3`. **No piper, no kokoro.**

- Input: `--text`, output WAV via `--out` (or `--out-dir` for chunked). Example (docs/usage.md): `audiocpp_cli --task tts --family pocket_tts --model models/PocketTTS-GGUF/english/pocket-tts-english-q8_0.gguf --backend cuda --request-sequence requests.json --out-dir outputs --metrics`.
- **Real-time-ish streaming out**: "Stream"-tagged TTS families (omnivoice, dots_tts, neutts, voxcpm2, confucius4_tts...) support `--mode streaming`; OmniVoice = "chunked pseudo streaming: emits one audio event per generated text chunk, then a merged final WAV" (`docs/models/omnivoice.md:43-46`). In C++, TTS streaming output arrives in `StreamEvent.audio_output` (`AudioBuffer{ sample_rate, channels, vector<float> samples }`) — feed those buffers to your own playback device.
- **Playback device API: none.** No portaudio/alsa output path in audio.cpp. We own the sink.

### d. Model management

- **Downloader: `tools/model_manager_v2.py`** (Python 3, stdlib urllib + optional HF token from `HF_TOKEN`/`HUGGING_FACE_HUB_TOKEN`/`~/.cache/huggingface/token`). Reads package metadata from `model_specs/*.json`, downloads from `https://huggingface.co/<repo>/resolve/<revision>/<path>` (`tools/model_manager_v2.py:165-166`) into a `models/` root.
  ```bash
  python3 tools/model_manager_v2.py list
  python3 tools/model_manager_v2.py install voxtral_realtime --models-root models
  ```
  Commands: `list`, `info`, `install`, `installed --json`, `sizes --json`, `uninstall`, `clean-partial`. Legacy converter path: `tools/model_manager_deprecated.py` (safetensors, needs torch) — avoid.
- **HF repos**: GGUF packages for core models at `audio-cpp/audio.cpp-gguf`; `FunAudioLLM/Fun-ASR-Nano-2512-GGUF`; community `mirek190/audio.cpp` (docs/model_manager.md:18-25).
- **Format**: GGUF primary (16-bit, Q8_0, Q4_K, INT8 per family — see docs/gguf.md), safetensors accepted by default loaders. Standalone GGUFs are self-contained (config + tokenizer embedded).
- **Model location**: `models/<family>/` layout; bundled VADs under `assets/framework/models/` (no download needed).
- **Spec system**: `model_specs/<family>.json` (schema-v1 variants in `model_specs_v1/`) are the source of truth for package links/options; `model_specs_demo.cpp` + `model_spec_download_demo.cpp` in examples/ show the C++ download/load API.

### e. Build system & API surface

- **CMake, C++17** (`set(CMAKE_CXX_STANDARD 17)` — CMakeLists.txt:8-9). ggml vendored in `external/ggml`.
- **Targets**: `engine_core` (OBJECT), `engine_model_*` (OBJECT, one per family), **`engine_runtime` (STATIC)** that folds in all selected model OBJECT libs + silero/marblenet VAD (CMakeLists.txt:1264-1266), `audiocpp_cli` (executable), `audiocpp_server` (HTTP server, app/server/), `audiocpp_gguf` (converter), plus tests.
- **Consumer linking**: `target_link_libraries(<app> PRIVATE engine_runtime ggml)` + OpenMP (CMakeLists.txt:1358-1368 pattern; tests do exactly this e.g. cli_request_options_test:2142). Include dirs: `include/` is PUBLIC on engine_runtime; the public runtime header is `include/engine/framework/runtime/session.h`. Runtime registry: `src/framework/runtime/registry.cpp` (family → loader mapping).
- **Composite builds** (README.md:204+): `-DAUDIOCPP_MODEL_SET=custom -DAUDIOCPP_MODELS=qwen3_asr,silero_vad,...` to compile only needed families. `silero_vad`/`marblenet_vad` always included (CMakeLists.txt:405).
- **Backends**: CPU (always), CUDA, HIP/ROCm, Vulkan, Metal via `ENGINE_ENABLE_*` cmake options. Backend picked by `--backend cpu|cuda|vulkan|metal|best` / `BackendConfig`.
- **Audio I/O backend: none.** No portaudio/alsa/pulse anywhere in the repo — only PCM/WAV in/out and the stdin pipe. (grep for portaudio/alsa/pulse hits only docs mentions and test fixtures.)
- **Server**: `audiocpp_server` HTTP adapter, keeps one model + session per model id, streaming via HTTP (WebUI embedded). Could be an alternative integration point (subprocess instead of linking).
- **Thread safety**: not documented as a promise; sessions are per-task-stateful. Plan for one session per stream and serialized event handling (the CLI/server model keeps one session per model id).

### f. Examples / demos

- `examples/ggml_simple_inference.cpp` — raw ggml tensor add/mul demo, not model-level.
- `examples/model_spec_demo/model_spec_demo.cpp` — loads a model spec and runs inference through the spec-backed API.
- `examples/model_spec_demo/model_spec_download_demo.cpp` — programmatic model download/install via spec (the C++-side downloader demo).
- `app/cli/` — the `audiocpp_cli` frontend; `app/cli/main.cpp`, `args.cpp`, `request.cpp`, `batch.cpp` — the streaming `--audio -` path is in `app/streaming/pcm_source.cpp` + `streaming.cpp`.
- `app/server/` — HTTP server with streaming (see app/server/README.md).
- **No mic→text or text→mic example exists** — closest are `tests/voxtral_realtime/voxtral_realtime_warm_bench.cpp` and `tools/streaming/measure_asr_streaming_client.py` / `measure_tts_streaming_client.py` (measure streaming ASR/TTS over the server).

### g. License

**Apache-2.0** (LICENSE: "Copyright 2026 ShugoAI LLC"). Permissive — fine to link from our daemon. No copyleft concern for the persona project (worth noting the vendored deps: ggml — MIT, sentencepiece — Apache-2.0, libyaml — MIT, cJSON — MIT, llama_tokenizer).

---

## Gotchas & risks

1. **No mic capture, no speaker playback in audio.cpp.** We must add capture (portaudio / ALSA / pipewire / webrtc-audio-processing) and playback ourselves. This is the biggest gap. Two clean patterns: (a) link `engine_runtime` and implement our own `AudioChunkReader` over portaudio; (b) subprocess `audiocpp_cli --mode streaming --audio -` and pipe s16le 16k PCM from our capture (simplest to start, adds process overhead + text handoff via stdout/JSON).
2. **Endpointing must be assembled by us.** Use silero_vad streaming (`SpeechEnd` event) + ASR `finish_stream()`. Only kroko_asr has built-in endpoint segmentation. No turn-taking state machine exists.
3. **Chunk sizes differ per model**: silero_vad wants 512 samples @ 16k; qwen3/nemotron/higgs prefer chunk-seconds; ASR default `preferred_audio_chunk_samples = 512`. Honor each session's `streaming_policy()` rather than one global chunk size. Both VAD and ASR want 16 kHz mono — mic must be resampled to 16 kHz if native rate differs.
4. **Version freshness**: very active project (release 0.6 on 2026-08-13, HEAD a day later). API (`IStreamingVoiceTaskSession`, StreamEvent) is stable in-tree but young — pin a release tag (e.g. `release-0.6`) rather than tracking HEAD.
5. **Heavy build**: ggml vendored; CUDA builds are heavy; CPU-only build works but some models are slow. `engine_runtime` static lib compiles ALL selected families' OBJECT libs into itself — a `custom` composite build keeps compile time and binary size down.
6. **Nix integration**: repo has `flake.nix` and `docs/build/nixos.md` — the persona flake should vendor audio.cpp via nix (e.g. a flake input or a `pkgs.stdenv.mkDerivation` wrapping the CMake build) rather than the raw `clang++ src/persona.cpp` line currently in flake.nix.
7. **Model download needs network + HF**: `model_manager_v2.py` requires network; standalone GGUF packages are self-contained once downloaded. Bundled VADs need no download. Note gguf.md's warning: whisper.cpp/llama.cpp GGUF files are NOT directly loadable (tensor-name mapping required).
8. **GGUF embedding of specs**: standalone GGUFs embed package specs; normal builds discover `model_specs/<family>.json` on disk at runtime (server README). Keep `model_specs/` accessible or use `AUDIOCPP_DEPLOYMENT_BUILD=ON`.
9. **C++17 required** (repo forces C++17; persona already uses C++17 — good). LLM-based models (qwen3, voxtral, supertonic) need 1.7B–4B weights; the lightest realistic TTS/ASR combo: `pocket_tts` (100M) + `fun_asr_nano` or `qwen3_asr-0.6B` + bundled silero_vad.
10. **Thread safety unstated** — serialize session access (one session per stream; don't share a session across threads).
