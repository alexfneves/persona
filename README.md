# persona — voice daemon on audio.cpp

A single Nix-built C++17 binary that turns a microphone into a continuous
voice channel for an AI agent: silero_vad endpointing → per-utterance
qwen3_asr streaming transcription → NDJSON on stdout → pocket_tts replies
spoken back. With `--agent pi` it spawns `pi --mode rpc` as a child and
submits every finished utterance as a `steer` prompt.

- **Build:** `nix build .#persona` (CPU) / `.#persona-vulkan` (GPU, AMD RADV).
- **Models:** `persona models install qwen3_asr pocket_tts` (one-time, from
  Hugging Face). silero_vad is bundled with the binary — no download.
- **Architecture + todo trail:** `docs/architecture.html`,
  `.pi/plans/2026-08-14-persona-daemon/` (plan, scout context, todos).

## Quickstart

```bash
nix develop                       # dev shell: persona is on PATH
persona models install qwen3_asr pocket_tts   # ~1.3 GB, one-time (HF download)
persona daemon                    # speak; each utterance -> NDJSON on stdout
```

The daemon emits a `ready` line, then one `speech.final` per finished
utterance (streaming `speech.partial` lines along the way). Send
`{"type":"stop"}` on stdin to shut down cleanly (exit 0). Model-less
scripted runs: `persona daemon --mic none --audio-fixture testdata/hello.wav
--models-root models` (mic-free test mode; needs models installed).

### pi hook-up

```bash
persona daemon --agent pi         # spawns `pi --mode rpc`, speaks replies
persona daemon --agent pi --pi-args '["--provider","openai","--model","gpt-4o"]'
persona daemon --agent pi --no-speak     # log replies instead of speaking
```

Every `speech.final` is submitted as a `prompt` with
`streamingBehavior:"steer"`; text replies are synthesized and played back
(`agent.reply.done {spoken:true}`). `--pi-args` takes a JSON array of
strings (space-separated also parses). **Real-pi end-to-end is a manual
test** (needs a working provider setup) — the RPC protocol contract is
covered automatically by `tests/agent_pi_smoke.sh` against
`tests/pi_stub.sh`. `PERSONA_PI_BIN` overrides the pi binary path.

## Model management

The catalog is the 47 shipped `model_specs/*.json` (audio.cpp's own spec
files, baked into the binary) — no live HF search. Downloading hits
Hugging Face via libcurl (`.part` resume, idempotent re-runs).

```bash
persona models search --task asr --streaming   # 7 streaming ASR families
persona models search --task tts               # 23 TTS families incl. pocket_tts
persona models search --task tts --q vox       # substring filter; --lang en too
persona models info qwen3_asr                  # default package, repo, all variants
persona models install qwen3_asr               # spec default: qwen3_asr_1_7b_q8_0
persona models install qwen3_asr --package qwen3_asr_0_6b_q8_0   # lighter (~1.2 GB)
persona models install pocket_tts              # English Q8_0 + alba voice (~130 MB)
persona models list                            # installed? + on-disk sizes
persona models uninstall pocket_tts
```

Models land in `$XDG_DATA_HOME/persona/models` by default
(`~/.local/share/persona/models`); point elsewhere with `--models-root
<dir>`. Gated repos need `HUGGING_FACE_HUB_TOKEN` set.

## Daemon protocol (NDJSON)

One JSON object per stdout line (daemon → agent), flushed after every line;
stdin (agent → daemon) is the command channel. All diagnostics go to
stderr — stdout is pure NDJSON.

```json
{"asr":"qwen3_asr","asr_package":"qwen3_asr_1_7b_q8_0","backend":"cpu","rate":16000,"tts":"pocket_tts","tts_package":"pocket_tts_english_q8_0","type":"ready","vad":"silero_vad"}
{"seq":1,"t_ms":19,"type":"speech.start"}
{"seq":1,"text":"Hello, world.","type":"speech.partial"}
{"seq":1,"chars":29,"duration_ms":2656,"empty":false,"text":"Hello, world. This is a test.","type":"speech.final"}
{"seq":1,"text":"Hello, world. This is a test.","type":"agent.sent"}
{"chars":15,"seq":1,"spoken":true,"type":"agent.reply.done"}
{"reason":"audio-fixture-eof","type":"shutdown"}
```

- **Out:** `ready`, `speech.start` / `speech.partial` / `speech.final` /
  `speech.error`, `agent.sent` / `agent.reply.done` / `agent.error`,
  `tts.done` / `tts.error`, `shutdown {reason: stdin-stop|signal|stdout-closed|audio-fixture-eof}`.
- **In:** `{"type":"tts","text":"...","seq":n}` (synthesize + play),
  `{"type":"stop"}` (graceful exit 0). Malformed lines are logged and skipped.
- **Endpointing model:** `Idle → Speaking → Finalizing`; a 30 s cap
  force-finalizes monologues; barge-in is sequential (queue depth 1);
  empty transcripts arrive with `"empty":true`.
- **Full spec:** `.pi/plans/2026-08-14-persona-daemon/plan.md` (§ Protocol).

### Tuning knobs

| Flag | Default | Meaning |
|---|---|---|
| `--vad-threshold` | `0.5` | silero_vad speech probability threshold, `(0,1]` |
| `--vad-min-silence-ms` | `1000` | silence that ends an utterance — endpoint latency ≈ this |
| `--vad-min-speech-ms` | `250` | minimum speech to start an utterance |
| `--utt-cap-s` | `30` | force-finalize utterances longer than this |
| `--asr-family` / `--asr-package` | `qwen3_asr` / spec default | select the ASR model (`models search --task asr`) |
| `--tts-family` / `--tts-package` | `pocket_tts` / spec default | select the TTS model (`models search --task tts`) |
| `--mic-device` / `--play-device` | PortAudio default | device index from `persona devices` |
| `--backend` | compiled-in | compute backend override (`cpu`/`vulkan`) |
| `--models-root` | `$XDG_DATA_HOME/persona/models` | model storage root |

## Backends: CPU vs Vulkan

The backend is a **build-time choice, exposed as package variants** — each
binary knows what it was built for:

```bash
nix build .#persona          # CPU (default)
nix build .#persona-vulkan   # Vulkan on AMD RADV / Mesa (ggml shaders)
```

`--backend` is an *override*, not a selector: a CPU binary refuses
`--backend vulkan` with a hint to build `.#persona-vulkan`. The `ready`
line echoes the compiled-in backend, and every engine session (VAD, ASR,
TTS) honors the parsed backend — the Vulkan variant runs the whole
daemon path on the GPU. Measured on the dev machine (AMD Strix Halo,
qwen3_asr 1.7B): the daemon's two-utterance fixture run (model load +
endpointing) completes in ≈ **3.3 s on Vulkan vs ≈ 7.3 s on CPU**
(≈ 2.2× faster), and the CPU-only `persona` build runs the same fixture
in ≈ 7.1 s.

## Building with Nix

- **No CMake/Makefile in this repo.** `flake.nix` is the only build config:
  the `audiocpp-lib` derivation runs audio.cpp's composite CMake build
  (pinned input `release-0.6`), then persona is a raw clang++ link against
  the static archives.
- **audio.cpp is pinned** by `flake.lock` (input `audiocpp`, tag
  `release-0.6`). Bump procedure: edit `audiocpp.url` in `flake.nix`, run
  `nix flake update audiocpp`, rebuild. Never point the input at HEAD.
- **`src = ./.` copies only git-tracked files** — `git add` new source
  files before `nix build`, or they silently won't be compiled.
- **Link line uses `-fopenmp=libgomp`** — bare `-fopenmp` fails on clang 21
  in this nixpkgs (`cannot find -lomp`).
- **`pkgs.libcurl` does not exist** in this nixpkgs branch — use
  `pkgs.curl` (`out` = libcurl.so, `dev` = headers). `pkgs.portaudio` has
  no `-dev` split (headers come from the main package).
- **Adding a dependency** = `flake.nix` `buildInputs` **and** the clang++
  link line. **Adding an audio.cpp model family** = `AUDIOCPP_MODELS` of
  `audiocpp-lib`, then rebuild both.

## Testing

```bash
devenv test                 # == bash tests/smoke.sh (full suite, needs models)
nix flake check             # sandbox smoke: selftest, selftest --vad, catalog
```

- `tests/smoke.sh` (run by `devenv test`) builds persona, then asserts
  selftest, catalog search, `listen` on `testdata/hello.wav`, daemon
  endpointing on `testdata/hello_hello.wav` (exactly 2 `speech.final`),
  the T13 model-selection/VAD-tuning block, and the pi-stub suite
  (`tests/agent_pi_smoke.sh`). Deterministic: no mic, no network.
- `nix flake check` runs `checks.<system>.smoke` — the model-free subset —
  inside the nix sandbox (no network, and `models/` is gitignored, so
  model-dependent asserts skip there; full model tests = `devenv test` on a
  machine with models). Manual: `PERSONA_BIN=result/bin/persona bash
  tests/flake_check.sh`.
- Orchestrated development uses one git worktree per todo
  (`git worktree add -b t<n> ...`), merged by the orchestrator — see
  `AGENTS.md`.

## Repository layout

| Path | What it is |
|---|---|
| `flake.nix` | the only build config (`persona`, `persona-vulkan`, `audiocpp-lib`, `audiocpp-cli`, `checks.smoke`, dev shell) |
| `src/` | verb dispatch (`main.cpp`), config, audio (capture/playback/ring/WAV), model (catalog/download/registry), pipeline (vad/stt/endpointer/tts), protocol (NDJSON), agent (pi RPC), daemon loop |
| `tests/` | `smoke.sh`, `flake_check.sh`, `pi_stub.sh`, `agent_pi_smoke.sh` |
| `testdata/` | 16 kHz mono fixture WAVs (`hello.wav`, `hello_hello.wav`, `tone_long.wav`) |
| `models/` | downloaded weights — gitignored; `persona models install ...` |
| `docs/` | `architecture.html` |
