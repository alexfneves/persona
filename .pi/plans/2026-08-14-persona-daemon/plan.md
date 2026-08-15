# Persona — voice daemon on audio.cpp

**Date:** 2026-08-14
**Status:** Draft (decisions flagged for user confirmation)
**Directory:** /home/alexfneves/gits/persona
**Dependency:** audio.cpp pinned to `release-0.6` (tag 2026-08-13; scratch clone at HEAD `73b21f9e` validated this plan)

## Intent

A native C++ CLI, `persona`, that turns a microphone into a continuous voice input channel for an AI agent: it captures audio, detects when the user stops speaking (endpointing), transcribes speech to text with a streaming ASR model, and hands the final text to the agent over a simple NDJSON protocol. TTS (text→audio streaming playback) is in scope as a secondary direction — the daemon can also speak the agent's replies. For the primary hook-up, `persona daemon --agent pi` spawns the pi coding agent in RPC mode as a child process: every finished utterance is submitted to pi as a `steer` prompt, and pi's text replies are spoken via TTS. Agent-agnostic NDJSON remains the default mode (external wrappers work unchanged).

The whole pipeline runs inside a single Nix-built binary that links audio.cpp's `engine_runtime` static library. No subprocess orchestration, no network service — just stdin/stdout and the audio device.

## User Story

As an AI agent operator, I want to run `persona daemon` as a sidecar process, speak normally, and have each finished utterance delivered to my agent as clean text — without pressing keys, and without half-uttered garbage.

## Behavior

### Happy Path (daemon)

1. `persona daemon` starts, loads silero_vad (bundled) + qwen3_asr 0.6B (downloaded), opens the default mic at 16 kHz mono.
2. Emits a `ready` NDJSON line on stdout.
3. User speaks. VAD emits SpeechStart → daemon starts an ASR utterance stream; streaming partials may be emitted as `speech.partial`.
4. User pauses. VAD emits SpeechEnd → daemon flushes trailing silence, calls `finish_stream()`, and emits one `speech.final` line containing the final text.
5. With `--agent pi`, the daemon submits the finished utterance to pi (RPC `prompt` with `streamingBehavior: steer`) and streams pi's reply text to TTS + playback as `text_delta` events arrive. Without `--agent`, an external wrapper (pipe consumer) may reply with a `tts` command on stdin. Emits `tts.done` (or `tts.error`).
6. `{"type":"stop"}` on stdin → graceful shutdown (VAD/ASR sessions finalized, PortAudio streams closed, exit 0).

### Edge Cases & Error Handling

- **Utterance too long** (default cap 30 s): force `finish_stream()` at the cap, emit final, continue. Prevents an infinite monologue from blocking the agent.
- **Speech during `finish_stream()`** (barge-in): v1 is strictly sequential — new speech is buffered in the mic ring buffer and picked up once the previous utterance is finalized. Documented latency cost (~one `finish_stream()`), accepted for v1.
- **No VAD event but long audio**: same 30 s cap triggers finalize.
- **ASR returns empty text**: emit `speech.final` with `"text":""` and `"empty":true` — the agent decides (most will ignore).
- **Model not installed**: daemon fails fast at startup with `persona models install qwen3_asr` hint; never auto-downloads at runtime.
- **No mic / device error**: exit nonzero with a message listing `persona devices`.
- **Malformed stdin line**: log to stderr, skip, keep running (never crash the daemon on agent-side junk).
- **stdout pipe closed** (agent died): daemon exits cleanly (SIGPIPE → graceful shutdown).

## Scope

### In Scope

- Nix flake: pinned audio.cpp `release-0.6` input, CPU composite build (`qwen3_asr` + `pocket_tts` + bundled silero_vad/marblenet), static libs + headers + assets copied into a reusable derivation.
- `persona` CLI verbs: `daemon`, `listen`, `tts`, `models`, `devices`, `selftest`, `--version`.
- Mic capture + speaker playback via PortAudio (16 kHz mono in; model-rate out).
- Endpointing: silero_vad streaming (`SpeechStart`/`SpeechEnd`) driving per-utterance ASR `start_stream`/`finish_stream`.
- NDJSON protocol on stdin/stdout (spec below).
- Model catalog + selection: `persona models search|list|info|install|uninstall` over the shipped `model_specs/*.json` — the fixed catalog of 47 supported families. audio.cpp does **not** search HF dynamically (`model_manager_v2.py` listings are local reads; only downloads hit the network); these spec files *are* the preselect. `--asr-family/--asr-package` and `--tts-family/--tts-package` select exactly one model per task at runtime.
- HF model downloader: `persona models install <family> [--package <id>]` (libcurl, HF resolve URLs from the spec, `.part` resume, gated-repo token support).
- pi integration: `persona daemon --agent pi` spawns `pi --mode rpc`, submits utterances, speaks replies (Decision 8).
- README with quickstart + Nix build notes.

### Out of Scope

- Turn-taking/barge-in smarts (queue depth 1, sequential) — v2.
- Voice-clone/persona-voice (speaker embeddings) — v2.
- Diarization, multi-mic, noise suppression (webrtc) — v2.
- Non-Linux backends (PortAudio abstraction keeps the door open, but Nix Linux is the only CI target).
- Hot-word wake ("hey persona") — v2.

## Effort & Quality

- **Level:** MVP → production-leaning. The daemon must be trustworthy enough to leave running; audio pipeline correctness (endpointing) is the quality bar. Not "critical" — no security/perf hardening pass.
- **Tests:** smoke level in-repo (a 16 kHz test WAV fixture + assertions on the NDJSON output; `nix flake check` runs `selftest` + `listen` on the fixture). No unit test framework in v1 — plain bash/nix check hooks.
- **Docs:** README + inline comments at the tricky spots (endpointer state machine, PortAudio callback). Full docs deferred.

## Constraints

- **C++17, clang, Nix** (existing conventions). No CMake in the persona repo itself — the flake drives both the audio.cpp CMake build and the persona build.
- **CPU backend only** in the default derivation. Hardware check: AMD GPU (`1002:1586`), no NVIDIA driver; `renderD128` exists so Vulkan *may* work, but CPU is the baseline. `ENGINE_ENABLE_*` all default OFF already — build stays deterministic.
- **One session per stream, serialized access** (audio.cpp gives no thread-safety guarantees). All session calls happen on a single pipeline thread; PortAudio callbacks only touch a lock-free ring buffer.
- **Chunk policy per session**, not global: honor `streaming_policy().preferred_audio_chunk_samples` (silero_vad wants 512 @ 16 kHz). We feed both sessions 512-sample frames; ASR re-chunks internally per its own policy via `process_audio_chunk`.
- Models live at `$XDG_DATA_HOME/persona/models` (default `~/.local/share/persona/models`), overridable with `--models-root`.

## Ideal State Criteria

### Core Functionality
- [ ] ISC-1: `nix build .#persona` succeeds with a pinned audio.cpp input (release-0.6), CPU-only composite.
- [ ] ISC-2: `persona selftest` loads the silero_vad runtime and prints a loader list — proves link+runtime.
- [ ] ISC-3: `persona listen <16k-mono-test.wav>` prints the correct transcript text.
- [ ] ISC-4: `persona models install qwen3_asr` downloads the GGUF into the models root from HF.
- [ ] ISC-5: `persona daemon` emits `ready`, then one `speech.final` per spoken utterance with the final text.
- [ ] ISC-6: Speaking two utterances back-to-back yields two `speech.final` lines (endpointing works).
- [ ] ISC-7: `{"type":"stop"}` on stdin exits the daemon with code 0.
- [ ] ISC-8: `persona tts --text "hello" --play` produces audible output (or `--out x.wav` without a device).

### Edge Cases
- [ ] ISC-9: A >30 s utterance is force-finalized and does not block the next one.
- [ ] ISC-10: Empty transcript yields `speech.final` with `"empty":true`, daemon stays up.
- [ ] ISC-11: Malformed stdin line is logged and skipped; daemon continues.
- [ ] ISC-12: Agent closes stdout; daemon exits cleanly (no crash, exit 0/nonzero as documented).
- [ ] ISC-13: `persona models search --task asr` lists qwen3_asr; `models install --package qwen3_asr_0_6b_q8_0` installs exactly that variant; `daemon --asr-package` loads it (visible in `ready`).
- [ ] ISC-14: `persona daemon --agent pi --no-speak` (against a stub `pi` script) submits a `speech.final` as an RPC `prompt` and emits `agent.sent` + `agent.reply.done` for the reply.

### Anti-Criteria
- [ ] ISC-A-1: No audio.cpp session is ever called from two threads concurrently.
- [ ] ISC-A-2: No blocking audio work happens inside the PortAudio callback.
- [ ] ISC-A-3: No HEAD-tracking of audio.cpp — the input is pinned by flake.lock to a release tag.

## Approach

**Single binary, linked engine_runtime.** The daemon is a normal C++ program; audio.cpp is a static library dependency built by the flake. This beats (a) subprocessing `audiocpp_cli` — extra process, text handoff through another pipe, harder to stream partials; and (b) running `audiocpp_server` — a whole HTTP stack we don't need for a local sidecar.

**Endpointing is assembled by us**: two streaming sessions (silero_vad + qwen3_asr) fed from one 16 kHz mono f32 ring buffer; VAD events drive the per-utterance ASR session lifecycle. Neither audio.cpp's CLI nor its server does turn-taking — this state machine is the product.

### Key Decisions

- **Decision 1 — Agent handoff: NDJSON on stdin/stdout.** One JSON object per line, UTF-8, line-buffered stdout. Composes with any wrapper (`persona daemon | your-agent`), zero network, trivially testable with `head -1`. **← user confirmation wanted (see Open Questions Q1).** Alternatives considered: HTTP/WebSocket (adds a server + auth surface), file drop (races, no backpressure), FIFO pair (works but NDJSON is a strict superset with less ceremony).
- **Decision 2 — Models:** ASR `qwen3_asr` 0.6B Q8_0 (streaming, multilingual, ~lightest realistic streaming ASR in-tree); VAD = bundled `silero_vad` (streaming, 512-sample chunks, no download); TTS `pocket_tts` English Q8_0 (100M — small and fast on CPU; upgrade path: `voxcpm2`/`qwen3_tts` if quality demands). Family/package ids are config, not hardcoded — `persona models install <family>` + `--asr-family`/`--tts-family` flags, with `--asr-package`/`--tts-package` picking the exact variant (e.g. `qwen3_asr_0_6b_q8_0` vs `qwen3_asr_1_7b_q8_0`).
- **Decision 3 — Mic/playback: PortAudio** (Nix `pkgs.portaudio`). Cross-platform enough, handles device enumeration (`persona devices`), and its callback model maps cleanly onto a lock-free ring buffer. audio.cpp has **no capture/playback** (validated) — this is the one gap we fill ourselves.
- **Decision 4 — Nix packaging:** audio.cpp as a flake input pinned to the release tag; derivation `audiocpp-lib` builds it with `-DAUDIOCPP_MODEL_SET=custom -DAUDIOCPP_MODELS=qwen3_asr,pocket_tts -DAUDIOCPP_DEPLOYMENT_BUILD=ON` (embeds model specs → self-contained runtime; validated in `CMakeLists.txt:14-29`), then copies `libengine_runtime.a`, `libggml.a`, `include/`, `assets/framework/models/` into `$out`. persona derivation links it. `result` symlink preserved.
- **Decision 5 — JSON: nlohmann/json** (`pkgs.nlohmann-json`) for protocol + spec parsing. audio.cpp's internal `engine::io::json` exists but is private; our own protocol shouldn't depend on library internals.
- **Decision 6 — Downloader:** `persona models install` is C++ + libcurl (`pkgs.libcurl`), parsing `model_specs/<family>.json` (embedded via deployment build) for `download.repo/revision` + `packages[].files[]`, fetching `https://huggingface.co/<repo>/resolve/<revision>/<file>` (URL scheme from `tools/model_manager_v2.py:165-166`). No torch, no python at runtime.
- **Decision 7 — Silence policy:** honor VAD defaults (`threshold` 0.5, `min_silence_duration_ms` 100, `min_speech_duration_ms` 250 — `docs/speech_analysis.md`), all overridable via request options in a future config. No hardcoded timers in the endpointer beyond the 30 s utterance cap.

**Decision 8 — pi integration: embed the RPC client.** `persona daemon --agent pi` spawns `pi --mode rpc` as a child with its own pipes (the user's tty stays persona's), speaks JSONL commands, consumes JSONL events (protocol from `docs/rpc.md`: LF-only framing, tolerate `\r\n`, never split on Unicode separators). Mapping: `speech.final` → `{"type":"prompt","message":<text>,"streamingBehavior":"steer"}` (queues while pi is mid-turn — voice-over works); replies arrive as `message_update` events with `assistantMessageEvent.type == "text_delta"` (accumulate per `contentIndex`; `message_end.message` is authoritative) — skip `thinking_*`/`toolcall_*`. Text deltas → TTS playback via the pipeline thread's command queue. `--no-speak` logs replies instead. Without `--agent`, the daemon is agent-agnostic NDJSON (the `| head -1` test story stays). Rejected for v1: a pi extension spawning persona — inverts parent/child and injecting spontaneous user messages from an extension is indirect; RPC mode exists precisely for embedding. New module: `src/agent/pi_rpc.h/.cpp`.

**Decision 9 — Model catalog is the shipped specs, not live HF search.** Verified: audio.cpp's `model_manager_v2.py` `list`/`info`/`sizes` are local reads of `model_specs/*.json`; only downloads hit the network. The 'preselect from HF' catalog = the 47 spec JSONs, shipped to `$out/share/persona/model_specs` and read with nlohmann (no dependency on audio.cpp private APIs). Spec shape: `family`, `display_name`, `description`, `category`, `status`, `tasks`, `modes` (offline/streaming), `languages`, `capabilities`, `package_defaults.download.{kind,repo,revision,gated}`, `packages[]` (`id`, `display_name`, `default`, `format`, `precision`, `target_directory`, `files[]`, `strip_prefix`). Verbs: `models search [--task asr|tts|vad] [--streaming] [--lang <code>] [--q <substr>]`, `models list` (available vs installed, sizes from disk manifest), `models info <family>`, `models install <family> [--package <id>]`, `models uninstall <family>`. Exactly one ASR + one TTS loaded at runtime, chosen via `--asr-family/--asr-package`, `--tts-family/--tts-package`; `ready` echoes actual ids.

### Architecture

```
persona binary (single process, 5 threads max)
┌─────────────────────────────────────────────────────────────┐
│ main thread: CLI dispatch, daemon control loop              │
│   └─ stdin reader (NDJSON commands)                         │
│                                                             │
│ mic thread (PortAudio callback)                             │
│   └─ SPSC ring buffer: 16 kHz mono f32                      │
│                                                             │
│ pipeline thread (ALL engine session calls — serialized)     │
│   ├─ silero_vad streaming session (long-lived)              │
│   ├─ qwen3_asr streaming session (per utterance)            │
│   └─ pocket_tts session (per tts command / reply)           │
│                                                             │
│ playback thread (PortAudio output callback)                 │
│   └─ FIFO of AudioBuffer → device                           │
│                                                             │
│ pi thread (only with --agent pi)                            │
│   └─ child `pi --mode rpc` on its own pipes                 │
│      speech.final → prompt(steer); text_delta → TTS queue   │
└─────────────────────────────────────────────────────────────┘
```

Modules (all new, in `src/`):

| File | Responsibility |
|---|---|
| `src/main.cpp` | verb dispatch (`daemon/listen/tts/models/devices/selftest/--version`) |
| `src/config.h/.cpp` | models root, family/package ids, VAD options, device ids, caps |
| `src/audio/capture.h/.cpp` | PortAudio input → ring buffer; `AudioChunkReader` adapter |
| `src/audio/playback.h/.cpp` | PortAudio output; `PlaybackQueue` fed by TTS |
| `src/audio/ringbuf.h` | lock-free SPSC ring buffer (f32) |
| `src/pipeline/vad.h/.cpp` | silero streaming session wrapper → `OnSpeech(Start|End)` callback |
| `src/pipeline/stt.h/.cpp` | per-utterance ASR streaming session wrapper → `OnPartial/OnFinal` |
| `src/pipeline/endpointer.h/.cpp` | state machine: idle → speaking → finalizing (the core) |
| `src/pipeline/tts.h/.cpp` | offline (v1) TTS run → `AudioBuffer` stream into playback queue |
| `src/protocol/ndjson.h/.cpp` | encode/decode NDJSON events/commands (nlohmann/json) |
| `src/agent/pi_rpc.h/.cpp` | `--agent pi`: spawn `pi --mode rpc` child (own pipes), submit `prompt`/`steer`, parse `message_update` → `text_delta` reply → TTS queue |
| `src/model/registry.h/.cpp` | `make_default_registry()` + load cached `ILoadedVoiceModel`s (ASR+VAD at startup, TTS lazily) |
| `src/model/download.h/.cpp` | spec-driven HF downloader (libcurl) |

### Data Flow

```
mic ─→ ringbuf ─→ [pipeline thread] ─→ silero_vad session (512-sample chunks)
                                        │ SpeechStart/SpeechEnd events
                                        ▼
                              endpointer state machine
                                        │ "speech started"
                                        ▼
                              qwen3_asr session.start_stream()
                              feed same 512-chunks → process_audio_chunk()
                                        │ partials (optional emit)
                                        │ SpeechEnd → finish_stream()
                                        ▼
                              TaskResult.text_output ─→ NDJSON "speech.final" → stdout

stdin "tts" ─→ pocket_tts run() ─→ AudioBuffer ─→ playback FIFO ─→ speaker
        tts.done/tts.error → stdout
```

**Key API surface (audio.cpp, validated in `include/engine/framework/runtime/`):**

```cpp
// registry + load (see app/cli/main.cpp:638)
auto registry = engine::runtime::make_default_registry();          // registry.h:43
auto model = registry.load(engine::runtime::ModelLoadRequest{
    .model_path = vad_assets_dir,        // bundled silero_vad dir
    .family_hint = "silero_vad", .options = {{"backend","cpu"}}});

// session lifecycle (session.h)
auto sess = model->create_task_session(
    {engine::runtime::VoiceTaskKind::Vad, engine::runtime::RunMode::Streaming},
    {engine::runtime::SessionOptions{backend, options}});
auto* st = dynamic_cast<engine::runtime::IStreamingVoiceTaskSession*>(sess.get());
st->start_stream(request);                       // streaming.cpp:124
auto ev = st->process_audio_chunk({16000,1,start_sample,std::move(samples)}); // streaming.cpp:66
auto res = st->finish_stream();                  // streaming.cpp:136 → TaskResult{text_output}
```

### Protocol (v1)

stdout (daemon → agent), one line per JSON object:
```json
{"type":"ready","asr":"qwen3_asr","tts":"pocket_tts","vad":"silero_vad","rate":16000}
{"type":"speech.start","seq":1,"t_ms":1234}
{"type":"speech.partial","seq":1,"text":"hello wo"}
{"type":"speech.final","seq":1,"text":"hello world","empty":false,"duration_ms":1830,"chars":11}
{"type":"speech.error","seq":1,"error":"asr session failed: ..."}
{"type":"agent.sent","seq":1,"text":"hello world"}          # --agent pi only
{"type":"agent.reply.done","seq":1,"chars":123,"spoken":true}
{"type":"tts.done","seq":2,"out_ms":3100}
{"type":"tts.error","seq":2,"error":"..."}
{"type":"shutdown","reason":"stdin-stop"}
```
stdin (agent → daemon):
```json
{"type":"tts","text":"Hello!","seq":2}
{"type":"stop"}
```

**`--agent pi` mapping (v1):** `speech.final` → `{"type":"prompt","message":<text>,"streamingBehavior":"steer"}`; parse pi stdout for `message_update` events with `assistantMessageEvent.type=="text_delta"` (accumulate per `contentIndex`, skip `thinking_*`/`toolcall_*`); `message_end.message` is authoritative → TTS. `--no-speak` logs replies instead of speaking.

## Dependencies

- `github:0xShug0/audio.cpp` `release-0.6` (flate input; Apache-2.0; vendored ggml MIT).
- `pkgs.portaudio` (capture + playback).
- `pkgs.libcurl` (HF downloads).
- `pkgs.nlohmann-json` (protocol + spec JSON).
- `pkgs.cmake`, `pkgs.ninja`, clang (build-time, via flake).
- models at runtime: qwen3_asr-0.6B q8_0 (~1.2 GB), pocket-tts english q8_0 (~100 MB), silero_vad bundled (free).
- `pi` binary at runtime (PATH), only for `--agent pi` — not a build dependency.

## Phases

- **Phase 0 (derisk, half-day):** build audiocpp_cli in the flake; pipe `arecord`/WAV fixture into `audiocpp_cli --task asr --mode streaming --audio -` and confirm a transcript. Confirms models + runtime before we write our own session code.
- **Phase 1 (core):** flake inputs + audiocpp-lib derivation; `persona selftest` + `persona listen <wav>`; downloader `persona models install`; mic capture + `persona devices`.
- **Phase 2 (daemon):** VAD + ASR session wrappers, endpointer state machine, `persona daemon` NDJSON out (text only). This is the headline deliverable.
- **Phase 3 (TTS + agent):** `persona tts` verb (wav + play), daemon `tts` command, playback queue, `tts.done`; then the pi RPC adapter (`--agent pi`, `src/agent/pi_rpc.*`) speaking replies.
- **Phase 4 (polish):** README, error paths (ISC-9..12), config plumbing, flake check hook.

## Risks & Open Questions

### Resolved with user (2026-08-14)

- **Q1 — pi integration:** Decision 8 (embedded RPC client, `--agent pi`). Chosen over a pi extension spawning persona.
- **Q2 — ASR default stays qwen3_asr-0.6B Q8_0;** the latency/quality tradeoff is now flag-level: `persona models search --task asr --streaming` + `--asr-family/--asr-package`.
- **Q3 — TTS default stays pocket_tts;** `persona models search --task tts` + `--tts-family/--tts-package` make voxcpm2/qwen3_tts one command away. No hardcoded single voice — the CLI can search, download, and select (Decision 9).

### Risk register (premortem)

| Risk | Mitigation |
|---|---|
| audio.cpp API churn post-0.6 | Pin release-0.6; our code touches only `session.h`/`registry.h`/`model.h` which are stable in-tree; upgrade = bump flake input + fix compile |
| Endpointing feels wrong in practice (missed/chopped endpoints) | Phase 2 exposes VAD options (`--vad-threshold`, `--vad-min-silence-ms`) so tuning is flag-level, not code |
| CPU ASR too slow → buffered gaps | Cap: if RTF > 1.0 for 3 consecutive utterances, log a warning suggesting Vulkan/1.7B tradeoff; functionality preserved (text arrives late) |
| HF download flakiness/partial files | curl `-C -` resume + `.part` rename; `models install` retries once; md5 not guaranteed by HF — trust size match |
| PortAudio mic is 48 kHz (USB cam mic) | PA opens 16 kHz requested rate; OS resamples. If device refuses, add a trivial linear resampler fallback (documented, small) |
| `finish_stream()` on ASR returns partial junk on chop | VAD `speech_pad_ms`/`min_silence_duration_ms` tuning + 30 s cap; emit anyway with `"empty"` flag |
| ggml/OpenMP linkage issues in Nix | Phase 0 CLI build exercises the exact link line first; link flags centralized in flake.nix |

**Accepted as-is:** sequential utterances (barge-in queue depth 1), CPU-only default, no wake-word.
