# AGENTS.md — Working in this repository

## Golden rules (read first — they cost tokens when ignored)

1. **Build with `nix build`.** `nix build .#persona` (or `.#audiocpp-cli`, `.#audiocpp-lib`). There is **no CMakeLists.txt or Makefile** in this repo — never run `clang++`, `cmake`, or `make` directly to build persona.
2. **Backend is a build-time choice, exposed as package variants.** `nix build` / `.#persona` = CPU (default); `.#persona-vulkan` = Vulkan (AMD RADV). Each binary knows its backend — no `--backend` flag needed at runtime (it's an override, e.g. forcing CPU on a Vulkan build).
2. **Test with `devenv test`.** This runs the `enterTest` script defined in `flake.nix`, which executes `tests/smoke.sh`. Do not invent ad-hoc test commands; verify your work with `devenv test`.
3. **Dependencies are added in `flake.nix`.** A system library goes into the `persona` derivation's `buildInputs` **and** its `clang++` link line. An audio.cpp model family goes into `AUDIOCPP_MODELS` of the `audiocpp-lib` derivation (then rebuild both). Never hand-edit link flags elsewhere.
4. **Tests live in `enterTest` → `tests/smoke.sh`.** To add a test, extend `tests/smoke.sh` (or add a script under `tests/` and call it from there). Keep tests deterministic: no mic, no network; use `--mic none --audio-fixture testdata/*.wav` for daemon tests.

## The project

`persona` is a C++17 voice daemon on top of audio.cpp: continuous mic capture → silero_vad endpointing (the endpointer state machine is the product) → per-utterance qwen3_asr streaming → NDJSON to an AI agent, with pocket_tts replies spoken back. It integrates with the **pi** coding agent via RPC mode (`persona daemon --agent pi`).

- Full architecture report: `docs/architecture.html`
- Plan / todos / scout context: `.pi/plans/2026-08-14-persona-daemon/`
- audio.cpp clone (read-only reference): `.scratch/audio.cpp` (pinned tag `release-0.6`)

## Common commands

```bash
nix build .#persona                                              # build (CPU backend)
nix build .#persona-vulkan                                       # build (Vulkan backend)
devenv test                                                      # run the smoke tests
result/bin/persona selftest                                      # loader list (no models needed)
result/bin/persona listen testdata/hello.wav                     # offline transcription
result/bin/persona daemon --mic none --audio-fixture testdata/hello_hello.wav   # scripted daemon run
result/bin/persona models search --task asr --streaming          # catalog browse
result/bin/persona models install qwen3_asr pocket_tts           # one-time download (network + HF)
result/bin/persona devices                                       # PortAudio device list
result/bin/persona daemon [--mic-device N]                       # live daemon (real mic)
```

## Repository layout (short)

| Path | What it is |
|---|---|
| `flake.nix` | The **only** build config: `persona`, `audiocpp-cli`, `audiocpp-lib` derivations + devenv shell (`enterTest`). |
| `src/main.cpp`, `src/config.*` | Verb dispatch table + flag parsing. |
| `src/audio/` | PortAudio capture (`capture.*`), playback (`playback.*`), SPSC ring (`ringbuf.h`), WAV read/write (`wav.*`). |
| `src/model/` | Catalog (`catalog.*`), HF downloader (`download.*`), model registry (`registry.*`). |
| `src/pipeline/` | `vad.*`, `stt.*`, `tts.*`, and the `endpointer.*` state machine. |
| `src/protocol/` | NDJSON wire types (`ndjson.*`). |
| `src/agent/` | `pi_rpc.*` — pi RPC client (`--agent pi`). |
| `src/daemon/` | `daemon.cpp` — pipeline loop, threads, signals. |
| `testdata/` | 16 kHz mono fixture WAVs: `hello.wav`, `hello_hello.wav`, `tone_long.wav`. |
| `tests/` | `smoke.sh` (run by `devenv test`), `pi_stub.sh` + `agent_pi_smoke.sh` (pi adapter tests). |
| `models/` | Downloaded weights — **gitignored**; install via `persona models install`. |
| `.scratch/` | audio.cpp clone — **gitignored**. |

## Non-negotiable engineering rules

- **Threading:** all audio.cpp session calls happen on the **pipeline thread** — never in a PortAudio callback, never from two threads. Callbacks only push/pop lock-free structures (ring buffer / playback queue) and never block.
- **daemon stdout is pure NDJSON** (one object per line, flushed); all diagnostics go to stderr.
- C++17, clang, no CMake in `src/`. Link line: `-fopenmp=libgomp` (bare `-fopenmp` fails on clang 21).

## Nix gotchas (learned the hard way)

- `src = ./.` in the flake copies only **git-tracked** files — `git add` new source files before `nix build`, or they silently won't be compiled.
- `pkgs.libcurl` does not exist in this nixpkgs branch — use `pkgs.curl` (`out` = libcurl.so, `dev` = headers).
- `pkgs.portaudio` has no `-dev` split — headers come from the main package.
- audio.cpp is pinned by `flake.lock` (input `audiocpp`, tag `release-0.6`). Bump = edit `audiocpp.url` in `flake.nix` + `nix flake update audiocpp` + rebuild. Never point the input at HEAD.
- Never commit model weights (`models/`) or the scratch clone (`.scratch/`).

## Audio-pipeline facts (verified — don't re-discover)

- **silero_vad:** feeds exactly **512-sample chunks @ 16 kHz**, contiguous (else it throws and the session stays broken); `prepare()` before `start_stream()`; tuning options ride `SessionOptions.options`.
- **qwen3_asr:** needs `audio_chunk_seconds=1.0` in the `start_stream` options (its internal window defaults to 30 s; 0.5 s hallucinates); `partial_text` carries **deltas** — accumulate for the cumulative partial; one session per utterance, never reused.
- **pocket_tts:** seed pinned to `0` (it randomizes otherwise); voice `alba`; output 24 kHz mono f32.
- **Endpointer:** `Idle → Speaking → Finalizing`; `--utt-cap-s` (30) force-finalizes monologues; barge-in = queue depth 1, sequential.

## When you change something

1. Add a source file → `git add` it first, then `nix build .#persona`.
2. Add a dependency → edit `flake.nix` (`buildInputs` + link line), rebuild.
3. Add a test → extend `tests/smoke.sh`, then run `devenv test`.
4. Run `devenv test` before committing — it must pass.
5. Commit with a clear message following the repo style (`Phase X T<n>: <summary>`).
