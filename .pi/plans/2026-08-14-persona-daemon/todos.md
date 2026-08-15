# Persona Daemon — Todos

> Plan: `.pi/plans/2026-08-14-persona-daemon/plan.md` (read it first — architectural decisions live there, incl. Decisions 8 & 9).
> Tag: `persona-daemon` · Level: MVP+ · Deps: audio.cpp pinned `release-0.6`, Nix, PortAudio, libcurl, nlohmann-json.
> Constraint repeated everywhere: **all audio.cpp session calls on one pipeline thread, never from the PortAudio callback.**

---

## Phase 0 — Derisk: CLI smoke test (before any persona C++)

### T0: Build audiocpp_cli in the flake and transcribe a fixture WAV

> ✅ **DONE (2026-08-14):** `nix build .#audiocpp-cli` succeeds; `testdata/hello.wav` (espeak-ng → ffmpeg 16 kHz mono s16le, 2.7 s, "Hello world. This is a test."); model installed via `model_manager_v2.py install qwen3_asr` → spec default `qwen3_asr_1_7b_q8_0` → **`--model models/Qwen3-ASR-1.7B-GGUF`** (NOT the 0.6B dir the plan assumed — spec marks 1.7B default). Offline: `text_output=Hello, world. This is a test.` · Streaming (stdin s16le): `text_output=Hello, world. This is a test.`

- **Files:** `flake.nix` (new derivation `audiocpp-cli`), add a 16 kHz mono test WAV under `testdata/` (generate with sox/ffmpeg or record: `arecord -f S16_LE -r 16000 -c 1 -t raw | ...`; a 2-3 s spoken word is fine, else `testdata/hello.wav`).
- **Reference:** audio.cpp `CMakeLists.txt` composite build (`AUDIOCPP_MODEL_SET=custom`, line 1202); CLI stdin path `app/streaming/streaming.cpp`; model download via `tools/model_manager_v2.py` (dev-only, python3 available in devenv).
- **Steps:**
  1. Add flake input: `audiocpp.url = "github:0xShug0/audio.cpp/release-0.6";` (+ `flake.lock` pin via `nix flake lock`).
  2. Derivation `audiocpp-cli`: cmake configure `-DCMAKE_BUILD_TYPE=Release -DAUDIOCPP_MODEL_SET=custom -DAUDIOCPP_MODELS=qwen3_asr,pocket_tts -DAUDIOCPP_DEPLOYMENT_BUILD=ON`, build target `audiocpp_cli`, copy `bin/audiocpp_cli` to `$out/bin`.
  3. Model download (dev): `python3 <audiocpp>/tools/model_manager_v2.py install qwen3_asr --models-root models` once.
  4. Smoke:
     ```bash
     result/bin/audiocpp_cli --task asr --mode offline --family qwen3_asr \
       --model models/Qwen3-ASR-1.7B-GGUF --backend cpu --audio testdata/hello.wav
     # and streaming via stdin:
     arecord -f S16_LE -r 16000 -c 1 -t raw -d 3 | result/bin/audiocpp_cli \
       --task asr --mode streaming --family qwen3_asr --model models/Qwen3-ASR-1.7B-GGUF \
       --backend cpu --audio - --input-format s16le --input-rate 16000 --input-channels 1
     ```
- **Acceptance (ISC-1 partial):** `nix build .#audiocpp-cli` succeeds; the offline WAV command prints the expected transcript; the streaming stdin command prints a transcript.
- **Do NOT:** build `full` model set; enable CUDA/Vulkan (CPU baseline).

---

## Phase 1 — Build + plumbing

### T1: `audiocpp-lib` derivation + persona links against it

> ✅ **DONE (2026-08-14):** `nix build .#audiocpp-lib` → **`/nix/store/32w51b8rgyxim2kgkcrss0888rwnf2qi-audiocpp-lib`**. All verifications pass: `$out/lib` has 7 `.a` archives; `$out/include/engine/framework/runtime/session.h` present (plus ggml.h/ggml-backend.h/ggml-alloc.h copied from `external/ggml/include`); `$out/assets/framework/models/silero_vad/silero_vad_16k.safetensors` present; `$out/share/persona/model_specs/qwen3_asr.json` present (47 specs total). Link smoke-test passed via a throwaway clang++ program calling `make_default_registry()` + `advertise_loaders()` → `loaders=4` (silero_vad, qwen3_asr, pocket_tts, marblenet_vad).
> **Confirmed persona link-line artifact set (ALL 7 archives, order matters):** `${audiocpp-lib}/lib/libengine_runtime.a ${audiocpp-lib}/lib/libggml.a ${audiocpp-lib}/lib/libggml-base.a ${audiocpp-lib}/lib/libggml-cpu.a ${audiocpp-lib}/lib/libsentencepiece.a ${audiocpp-lib}/lib/libcjson_vendor.a ${audiocpp-lib}/lib/libyaml_vendor.a` — include dir: `-I${audiocpp-lib}/include`. **OpenMP: use `-fopenmp=libgomp` (NOT bare `-fopenmp`)** — clang 21 driver in nixpkgs stdenv fails bare `-fopenmp` with `cannot find -lomp`; libgomp ships in the stdenv gcc lib dir (`-fopenmp=libgomp` forces it).
> **CMake flags that needed adjusting:** embedded `-DENGINE_BUILD_EXAMPLES=OFF -DENGINE_BUILD_TESTS=OFF` (both default OFF already — no break). `ENGINE_ENABLE_LLAMAFILE` left at default ON (omitting it does NOT error; the T0 CLI build already used it). Two non-cmake derivation tweaks: `dontUseCmakeBuildDir = true` (build in-tree so installPhase finds artifacts from one cwd), and ggml headers must be copied separately (`cp -r external/ggml/include/* $out/include/`) since public engine headers `#include "ggml.h"` etc.

- **Files:** `flake.nix`, `src/persona.cpp` (keep hello-world until T2).
- **Pattern:**
  ```nix
  audiocpp-lib = pkgs.stdenv.mkDerivation {
    name = "audiocpp-lib";
    src = inputs.audiocpp;
    nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];
    buildInputs = [ pkgs.openssl ]; # if needed; omit unless cmake complains
    cmakeFlags = [
      "-DCMAKE_BUILD_TYPE=Release"
      "-DAUDIOCPP_MODEL_SET=custom" "-DAUDIOCPP_MODELS=qwen3_asr,pocket_tts"
      "-DAUDIOCPP_DEPLOYMENT_BUILD=ON"
      "-DENGINE_ENABLE_LLAMAFILE=OFF"   # optional: leaner; verify no compile break
      "-DENGINE_BUILD_EXAMPLES=OFF" "-DENGINE_BUILD_TESTS=OFF"
    ];
    installPhase = ''
      mkdir -p $out/lib $out/include $out/assets $out/share/persona/model_specs
      cp libengine_runtime.a libggml.a libggml-base.a libggml-cpu.a $out/lib/
      cp external/sentencepiece/src/libsentencepiece.a libcjson_vendor.a libyaml_vendor.a $out/lib/
      cp -r include/* $out/include/
      cp -r assets/framework/models $out/assets/
      cp model_specs/*.json $out/share/persona/model_specs/   # Decision 9 catalog (also embedded via deployment build)
    '';
  };
  ```
  **T1a:** discover the real artifact names in the Phase-0 build dir (`find build -name "*.a"`) and hardcode them — do not leave `<real .a names>` fake.
  > ✅ **Phase-0 result (from `nix build .#audiocpp-cli -L` log, 2026-08-14):** static archives produced by the composite build are
  > `libengine_runtime.a` (top), `ggml/src/libggml.a`, `ggml/src/libggml-base.a`, `ggml/src/libggml-cpu.a`, `external/sentencepiece/src/libsentencepiece.a`, `libcjson_vendor.a`, `libyaml_vendor.a`.
  > CLI link line is exactly `engine_runtime + ggml` (+ OpenMP, `ENGINE_ENABLE_OPENMP` defaults ON) — the T1 pattern's `libengine_runtime.a + libggml.a` should suffice; the base/cpu/sentencepiece/vendor archives are pulled in by the `ggml` target.
  > **T1a confirmed in the real build (2026-08-14):** the `find . -name '*.a'` sweep in `audiocpp-lib`'s installPhase produced EXACTLY these 7 archives in `$out/lib`: `libengine_runtime.a`, `libggml.a`, `libggml-base.a`, `libggml-cpu.a`, `libsentencepiece.a`, `libcjson_vendor.a`, `libyaml_vendor.a`. The smoke-test link (which mirrors the T2 line) needed ALL 7 on the clang++ command line (archive order as listed), plus `-fopenmp=libgomp`.
- **Constraints:** `AUDIOCPP_MODELS` must be the exact family names from `model_specs/*.json` (silero_vad/marblenet always included — CMakeLists.txt:405); pin via flake.lock, never HEAD. **Ship `model_specs/*.json` to `$out/share/persona/model_specs`** (Decision 9 — this is the searchable catalog).
- **Acceptance (ISC-1):** `nix build .#audiocpp-lib`; `ls $out/lib/libengine_runtime.a $out/lib/libggml.a`; `ls $out/include/engine/framework/runtime/session.h`; `ls $out/assets/framework/models/silero_vad`; `ls $out/share/persona/model_specs/qwen3_asr.json`.

### T2: persona build wired + `selftest` verb

> ✅ **DONE (2026-08-14):** `nix build .#persona` succeeds; `result/bin/persona selftest` prints `registered_loaders=4` (marblenet_vad, pocket_tts, qwen3_asr, silero_vad) + `silero_vad_loaded=yes`, exit 0. Dispatch table in `src/main.cpp` (selftest + reserved listen/tts/models/devices/daemon stubs; unknown verb → usage + exit 1); `src/config.*` (models_root/specs_dir/backend + flag parsing); `src/model/registry.*` (`make_runtime`: VAD required, ASR soft-fail). Link line = T1 artifact set + `-fopenmp=libgomp` + `-DPERSONA_SPECS_DIR="${audiocpp-lib}/share/persona/model_specs"`; `$out/assets` copied for runtime VAD resolution (`/proc/self/exe`-relative, `PERSONA_ASSETS_DIR` override verified).

- **Files:** `flake.nix` (persona derivation), `src/main.cpp` (verb dispatch), `src/model/registry.h/.cpp` (first cut: registry + load silero_vad).
- **Pattern (registry, modeled on `app/cli/main.cpp:638`):**
  ```cpp
  // src/model/registry.h
  struct Runtime {
      engine::runtime::ModelRegistry registry = engine::runtime::make_default_registry();
      std::unique_ptr<engine::runtime::ILoadedVoiceModel> vad_model;
      std::unique_ptr<engine::runtime::ILoadedVoiceModel> asr_model;
  };
  Runtime make_runtime(const Config& cfg); // loads silero_vad (assets dir) + asr (models root)
  ```
- **persona derivation:**
  ```nix
  persona = pkgs.stdenv.mkDerivation {
    name = "persona";
    src = ./.;
    buildInputs = [ audiocpp-lib pkgs.portaudio pkgs.libcurl pkgs.nlohmann-json pkgs.clang ];
    buildPhase = ''
      clang++ -O2 -std=c++17 src/*.cpp src/audio/*.cpp src/model/*.cpp src/pipeline/*.cpp src/protocol/*.cpp src/agent/*.cpp \
        -I${audiocpp-lib}/include -I${pkgs.nlohmann-json}/include \
        ${audiocpp-lib}/lib/libengine_runtime.a ${audiocpp-lib}/lib/libggml.a \
        -fopenmp -lportaudio -lcurl -o persona
    '';
    installPhase = "mkdir -p $out/bin && cp persona $out/bin/ && cp -r ${audiocpp-lib}/assets $out/assets";
  };
  ```
  (Adjust per actual `.a` names from T1a. Persona's own specs dir: read from `${audiocpp-lib}`'s shipped `$out/share/persona/model_specs` — pass via `-DPERSONA_SPECS_DIR` or find at runtime relative to `argv[0]`/`PERSONA_SPECS_DIR` env.)
- **`selftest`:** load silero_vad via `make_runtime()`, call `registry.advertise_loaders()`, print family list + number of loaders to stdout, exit 0.
- **Constraints:** C++17, clang; no CMake in persona repo; models root default `$XDG_DATA_HOME/persona/models` (env override `PERSONA_MODELS_ROOT`); catalog dir override `PERSONA_SPECS_DIR`.
- **Acceptance (ISC-2):** `nix build .#persona && result/bin/persona selftest` prints ≥1 loader containing `silero_vad`, exit 0.

### T3: `persona listen <wav>` — offline ASR transcription

> ✅ **DONE (2026-08-14):** `nix build .#persona` succeeds. **`result/bin/persona listen --models-root models testdata/hello.wav` → `Hello, world. This is a test.`** (exit 0) — matches the T0 CLI transcript exactly. **`ffmpeg -i testdata/hello.wav -f s16le -ac 1 -ar 16000 - | persona listen --models-root models --stdin` → same transcript.** WAV variants verified: float32 PCM, stereo→mono downmix, 44.1 kHz → 16 kHz linear resample (all transcribe correctly). `--mic` prints "not implemented yet (planned for the T6 todo)" + exit 1.
> **API surprises vs the sketch:** none — the session API matched `session.h` exactly (`BackendConfig{type,device,threads}` in `SessionOptions.backend`; `BackendType::Cpu`; `IOfflineVoiceTaskSession::run(TaskRequest{audio_input})` → `res.text_output->text`). Two practical notes: (1) `registry.load` on a missing model path throws eagerly — caught in `make_runtime` (soft fail, `asr_loaded=no`), and `listen` prints the install hint; (2) threads: left to `std::thread::hardware_concurrency()` (CLI default was 4; "auto" here). `-fopenmp=libgomp` still required.
> **Model-path resolution:** `resolve_asr_model_dir(cfg)` reads `$PERSONA_SPECS_DIR/qwen3_asr.json` → default package's `target_directory` (`Qwen3-ASR-1.7B-GGUF`) under models root; falls back to `models_root/qwen3_asr` if the spec read fails. Proven: model only exists at `models/Qwen3-ASR-1.7B-GGUF` (fallback dir doesn't exist), and it loaded. `nlohmann-json` introduced now (spec reading; `pkgs.nlohmann_json` in buildInputs + `-I`).
> **New files:** `src/audio/wav.h/.cpp` (RIFF/fmt/data parse, 16-bit PCM + float32, mono downmix, linear resample → 16k mono f32, ~150 lines), `src/listen.cpp` (verb: `<wav>`/`--stdin`/`--mic` stub + shared offline run). `src/model/registry.cpp` extended. `main.cpp` dispatch row + forward decl.
- **Pattern — session usage (from `app/streaming/streaming.cpp:66-140` and `session.h`):**
  ```cpp
  auto sess = asr_model->create_task_session(
      {engine::runtime::VoiceTaskKind::Asr, engine::runtime::RunMode::Offline},
      {engine::runtime::SessionOptions{backend, {{"backend","cpu"}}}});
  auto* off = dynamic_cast<engine::runtime::IOfflineVoiceTaskSession*>(sess.get());
  engine::runtime::TaskRequest req;
  req.audio_input = engine::runtime::AudioBuffer{16000, 1, read_wav_f32(path)};
  auto res = off->run(req);
  std::cout << res.text_output->text << "\n";  // guard optional
  ```
- **Reference:** `app/cli/main.cpp` arg handling for `--model`, `--backend`, `--family`; WAV read: 16-bit PCM + WAV header parse (~60 lines, no lib).
- **Acceptance (ISC-3):** `persona listen testdata/hello.wav` prints the expected transcript text; exit 0. Also `persona listen --stdin` (raw s16le 16k on stdin) works — reused by tests.

### T4: Model catalog verbs — `search` / `list` / `info` (Decision 9)

> ✅ **DONE (2026-08-14):** `nix build .#persona` succeeds. `src/model/catalog.*` (load_catalog/find_spec, nlohmann parse of all 47 shipped specs) + `src/models.cpp` (`models` verb with search/list/info + T5 install/uninstall stubs) + one dispatch row in `src/main.cpp`. Exit codes: 0 success, 1 user error/empty, 2 catalog-unreadable.
> **Verified outputs:** `persona models search --task tts` → 23 families incl. `pocket_tts`, `qwen3_tts`, `voxcpm2` (exit 0). `persona models search --task asr --streaming` → exactly the 7 streaming families (qwen3_asr, nemotron_asr, voxtral_realtime, higgs_audio_stt, kroko_asr, parakeet_tdt, sense_asr). `persona models info qwen3_asr` → marks `[default] qwen3_asr_1_7b_q8_0`, shows `download: audio-cpp/audio.cpp-gguf @ main`. `persona models search --task nope` → `models search: no models match the given filters` + exit 1. `persona models list --models-root models` → `qwen3_asr yes 2.3G` (du -sb actual = 2473010434 B; 47 rows, all other families `no -`). `--models-root` global flag works for `models list`. `--q` substring + `--lang en` prefix filters verified.
> **Spec-shape surprises:** (1) `package_defaults.download` is **absent/null in 6 specs** (fun_asr_nano, glm_tts, hviske_asr, outetts, sense_asr, vietneu_tts) — `info` prints `download: (per-package - see packages below)` and T5 must read per-package `download` overrides (e.g. safetensors packages point at different repos; `pocket_tts_english_safetensors` is `gated:true` at package level). (2) All 47 shipped specs have object packages (no string entries in practice) but the parser skips non-object entries defensively anyway. (3) `default` may be absent/null — `value("default", false)` handles both. (4) The todos sketch's `std::span<const Spec>` is C++20 — under the C++17 constraint `find_spec` takes `const std::vector<Spec>&` instead.

- **Files:** `src/model/catalog.h/.cpp`, `src/main.cpp` (verb dispatch).
- **Spec parsing with nlohmann** — read `$PERSONA_SPECS_DIR/*.json` (shipped `model_specs/`, Decision 9). Fields per spec: `family`, `display_name`, `description`, `category`, `status`, `tasks[]`, `modes[]`, `languages[]`, `package_defaults.download.{repo,revision,gated}`, `packages[]` (`id`, `precision`, `format`, `default`, `target_directory`, `files[]`).
  ```cpp
  // src/model/catalog.h
  struct Package { std::string id, precision, format, target_directory;
                   std::vector<std::string> files; bool is_default = false; };
  struct Spec {
      std::string family, display_name, category, description;
      std::vector<std::string> tasks, modes, languages;
      std::vector<Package> packages;
      std::optional<std::string> repo, revision; bool gated = false;
  };
  std::vector<Spec> load_catalog(const std::filesystem::path& specs_dir);
  const Spec* find_spec(std::span<const Spec>, std::string_view family);
  ```
- **Verbs:**
  - `persona models search [--task asr|tts|vad|...] [--streaming] [--lang <code>] [--q <substr>]` → aligned table: family / display / task / modes / langs / default package (precision). `--streaming` filters `modes` containing `streaming`; `--q` matches family+display_name+description (case-insensitive).
  - `persona models list` → per family: available + installed? (models root dir exists → show sizes from manifest on disk, else `-`).
  - `persona models info <family>` → full spec: tasks/modes/langs, each package with `precision/format/default`, download repo/revision (gated warning if `gated:true`).
- **Constraints:** catalog read is pure data (no network, no audio.cpp private APIs); exit 1 with a clear message on empty search; family arg never `..`/path-injectable (used only as a lookup key).
- **Acceptance (ISC-13 partial):** `persona models search --task tts` shows `pocket_tts`, `qwen3_tts`, `voxcpm2`, …; `persona models search --task asr --streaming` shows only the 7 streaming families (qwen3_asr, nemotron_asr, voxtral_realtime, higgs_audio_stt, kroko_asr, parakeet_tdt, sense_asr); `persona models info qwen3_asr` marks `qwen3_asr_1_7b_q8_0` as default.

### T5: `persona models install <family> [--package <id>]` + `uninstall` (HF downloader)

> ✅ **DONE (2026-08-15):** `src/model/download.h/.cpp` (libcurl, no python/torch at runtime) + `install`/`uninstall` wired into `src/models.cpp`; `pkgs.curl` added to the flake buildInputs + `-lcurl` on the link line (`pkgs.libcurl` does NOT exist in the devenv-nixpkgs patched rolling branch — `pkgs.curl` carries `libcurl.so` in `out` and headers in `dev`). Catalog extended: `Package::strip_prefix` + optional `Package::download` (per-package override, T4's finding) parsed in `catalog.cpp`.
> **Verified (real downloads into `models/`):**
> - `persona models install pocket_tts --models-root models` → `installed pocket_tts pocket_tts_english_q8_0 (134051128)` (127,856,704 B gguf + 6,194,424 B alba.safetensors), manifest at `models/pocket_tts/.persona-manifest.json`.
> - Re-run → no-op: single stdout line `installed ... (134051128)`, no downloads, exit 0. Stdout stays clean (only the final line; progress to stderr).
> - **Resume:** truncated the installed gguf to 10 MB, renamed to `.part`, dropped the manifest → re-run resumed from 10 MB (progress started at 18.0M), final = 127,856,704 B again, valid `GGUF` magic, no `.part` left.
> - **Package selection:** `persona models install qwen3_asr --package qwen3_asr_0_6b_q8_0` → `installed qwen3_asr qwen3_asr_0_6b_q8_0 (1151272416)` into `models/Qwen3-ASR-0.6B-GGUF/` (not the 1.7B default dir).
> - **Failure path:** bogus repo → 2 attempts (initial + retry), `.part` left behind, no manifest, exit 2. Same for the no-download-source case (modified spec with neither spec nor package download info) → `no download source in spec` exit 2. (All 47 shipped specs have a source for every package — the exit-2 path needed a doctored spec to test.)
> - **Idempotent without manifest:** existing files with matching remote HEAD size are skipped (no `downloading` lines) and the manifest is (re)written.
> - `--force` redownloads. `--package bogus_pkg` → exit 1 + `persona models info <family>` hint; `bogus_family` → exit 1 + search hint.
> - `persona models list --models-root models` now prefers manifest sizes: `pocket_tts yes 127.8M`, `qwen3_asr yes 1.1G`.
> - `uninstall pocket_tts` removes target dir + manifest + empty ancestor dirs (`PocketTTS-GGUF`), exit 0; reinstall for later todos (TTS needs it).
> - **Regression:** `persona listen` (offline + stdin) still transcribes, `selftest` OK, `models search --task tts` finds pocket_tts.
> **Download-info spec surprises beyond T4's findings:** none new — per-package `download` overrides are exactly where T4 flagged them (safetensors packages, incl. gated `kyutai/pocket-tts`), `revision` defaults to `main` when omitted. `Authorization: Bearer` header is attached whenever `HF_TOKEN`/`HUGGING_FACE_HUB_TOKEN` is set (gated or not; verified harmless on public repos). `strip_prefix` applied per model_manager_v2.py `stripped_path()` semantics.
> **Final models/ state:** `pocket_tts` (reinstalled, persona manifest), `qwen3_asr` 0.6B (persona manifest) + 1.7B (from T0/T3 via model_manager, no persona manifest — 1.7B stays the registry default).

- **Files:** `src/model/download.h/.cpp`, `src/main.cpp` (verb).
- **Flow:**
  1. `find_spec(specs, family)` → default package = the one with `"default":true` (or `--package <id>`).
  2. Build URLs from spec `package_defaults.download`: `"https://huggingface.co/" + repo + "/resolve/" + revision + "/" + file` (URL scheme from `tools/model_manager_v2.py:165-166`; `strip_prefix` on target paths).
  3. Download each file into `<models_root>/<target_directory>/…` via libcurl:
     ```cpp
     curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
     curl_easy_setopt(curl, CURLOPT_WRITEDATA, fh);       // .part file
     curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)fs::file_size(part));
     // success: fs::rename(part, final); failure: keep .part for resume (one retry)
     ```
  4. Write a small manifest `<models_root>/<family>/.persona-manifest.json` (`{family, package, files:[{path,bytes}]})` — powers `models list` sizes offline.
- **Constraints:** no python/torch at runtime; gated repos → send `HF_TOKEN`/`HUGGING_FACE_HUB_TOKEN` env as `Authorization: Bearer` header when set; `install` is idempotent (skip file if exists and size matches manifest/spec); interrupted download resumes.
- **Acceptance (ISC-4, ISC-13 partial):** `persona models install qwen3_asr` downloads `Qwen3-ASR-0.6B-GGUF/qwen3-asr-0.6b-q8_0.gguf` + siblings into models root; `install --package qwen3_asr_0_6b_q8_0` picks the 0.6B variant; re-run is a no-op; interrupted download resumes; `persona models install pocket_tts` works; `uninstall` removes the family dir.

### T6: Mic capture + `persona devices`

> ✅ **DONE (2026-08-15):** `nix build .#persona` succeeds. **`result/bin/persona devices`** prints the table below (exit 0). **`result/bin/persona listen --mic --models-root models`** captures 3 s and runs the T3 offline ASR path: `42362 samples (2.65 s)` → transcript (see below). Explicit device selection `--mic-device 0` (FHD Camera Microphone) also verified: `48048 samples (3.00 s)`. Error paths: invalid index → `portaudio: invalid device index 99 (device count is 10)  run \`persona devices\`...` + exit 1; non-numeric `--mic-device abc` → clear message + exit 1. Regression: `listen testdata/hello.wav` → `Hello, world. This is a test.`, `listen --stdin` same, `selftest` OK, `models list`/`search --task tts` green.
> **Devices table (real hardware, `persona devices`, exit 0):**
> ```
> Audio devices (10):
>   idx in  out rate     name
>   0   2   0   44100    FHD Camera Microphone: USB Audio (hw:0,0)
>   1   0   8   44100    HD-Audio Generic: HDMI 0 (hw:1,3)
>   2   0   8   44100    HD-Audio Generic: HDMI 1 (hw:1,7)
>   3   0   8   44100    HD-Audio Generic: HDMI 2 (hw:1,8)
>   4   0   8   44100    HD-Audio Generic: HDMI 3 (hw:1,9)
>   5   2   0   48000    HD-Audio Generic: SN6186 Analog (hw:2,0)
>   6   128 0   48000    sysdefault
>   7   2   0   44100    spdif
>   8   128 128 44100    pipewire
>   9   128 128 44100    default  [default input]  [default output]
> ```
> **`listen --mic` outcome:** the room was quiet (no speech); the 0.6B ASR model hallucinated short phrases on the ambient noise — `我就好说。` (run 1, default mic) and `ya sabes no pretendes nada.` (run 2, `--mic-device 0`). Different phrase each run is the signature of noise hallucination rather than speech; the task's acceptance (no crash, samples count > 0) is met. Empty-transcript runs would also be acceptable per the task.
> **PortAudio in this nixpkgs (devenv-nixpkgs rolling):** `pkgs.portaudio` = `portaudio-190700_20210406`, **no `-dev` split** — headers (`include/portaudio.h`) and `lib/libportaudio.so` both ship in `out`. flake change = `buildInputs += pkgs.portaudio`, link line gains `-I${pkgs.portaudio}/include -lportaudio`.
> **PA API surprises:** (1) the `userData` pointer passed to `Pa_OpenStream` is the only way to reach the ring buffer from the callback — no `Pa_OpenStreamSetUserData` exists (my first draft invented one; fixed to pass `&cb_ctx_` directly). (2) `Pa_IsFormatSupported` is advisory — the real float32/int16 negotiation is retrying `Pa_OpenStream`. (3) PA init spews ALSA/JACK probe warnings to stderr on this machine (29 lines, `ALSA lib ...` / `jack server is not running`); harmless, stderr only, NDJSON protocol out (Phase 2) is unaffected. (4) `paFramesPerBufferUnspecified` worked as designed — the callback size is host-chosen (ALSA delivered frames as expected).
> **New files:** `src/audio/ringbuf.h` (header-only SPSC, monotonic-counter design, capacity 8192 = 512 ms @ 16 kHz, drop-on-overflow push, no alloc in push/pop — verified with a standalone stress test: 8192 pushed / 11808 dropped / FIFO order), `src/audio/capture.h/.cpp` (PortAudio input: static-init guard `Pa_Initialize` once / `Pa_Terminate` at exit; `open_default_mic`/`open_mic(index, rate)`; float32→int16 fallback; callback is a private static member pushing only — ISC-A-2), `src/devices.cpp` (verb). `src/config.h/.cpp` gained `--mic-device <index>` (Config::mic_device, -1 = PA default); `src/main.cpp` dispatch row; `src/listen.cpp` `--mic` implemented (drains the ring every 20 ms into a 3 s buffer, then runs the existing T3 offline path).
- **Files:** `src/audio/ringbuf.h` (SPSC f32), `src/audio/capture.h/.cpp`, `src/devices.cpp` (verb).
- **Pattern (PortAudio input):**
  ```cpp
  // callback must NOT block; push float samples into ringbuf
  static int pa_cb(const void* in, void*, unsigned long n,
                   const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void* user) {
      auto* r = static_cast<RingBuffer<float>*>(user);
      const float* s = static_cast<const float*>(in);
      for (unsigned long i = 0; i < n; ++i) if (!r->push(s[i])) break; // drop on overflow
      return paContinue;
  }
  Pa_OpenDefaultStream(&st, 1, 0, paFloat32, 16000, 0 /* paFramesPerBufferUnspecified */, pa_cb, &ring);
  Pa_StartStream(st);
  ```
- **Constraints:** callback-only work is non-blocking (ISC-A-2); capture at 16 kHz mono (PA resamples if the device is 48 kHz); if `paFloat32` unsupported fall back to `paInt16` + convert.
- **`persona devices`:** `Pa_GetDeviceInfo(i)` loop → name/rate/channels table (input and output).
- **Acceptance:** `persona devices` lists the USB "FHD Camera Microphone" + HDMI output; `persona listen` gains `--mic` mode that captures 3 s and prints the transcript (manual verification on real hardware — the automated part is: no crash, samples count > 0).

---

## Phase 2 — Daemon (the headline)

### T7: silero_vad streaming wrapper

- **Files:** `src/pipeline/vad.h/.cpp`.
- **Pattern:**
  ```cpp
  class VadSession {
  public:
      struct Events { std::function<void()> on_speech_start; std::function<void()> on_speech_end; };
      explicit VadSession(engine::runtime::ILoadedVoiceModel& vad_model, Events ev);
      void start();   // create_task_session({Vad, Streaming}, opts); start_stream({});
      void feed(const std::vector<float>& mono_f32_16k, int64_t start_sample); // process_audio_chunk
  private:
      std::unique_ptr<engine::runtime::IVoiceTaskSession> sess_;
      engine::runtime::IStreamingVoiceTaskSession* stream_ = nullptr;
      Events ev_;
      bool speaking_ = false;
  };
  ```
- **Core logic:** chunk = 512 samples @ 16 kHz (from `streaming_policy().preferred_audio_chunk_samples` — honor it, don't hardcode); scan `StreamEvent.voice_activity` for `Kind::SpeechStart/SpeechEnd`; on transition call the event callback. Keep the session alive for daemon lifetime (stream open); call `finish_stream()` only on shutdown.
- **Reference:** `session.h` (`VoiceActivityEvent` ~line 181), `app/streaming/streaming.cpp` feed loop.
- **Acceptance (ISC-5 partial):** unit-smoke via `persona selftest --vad` feeding a synthetic tone (1 s on, 1 s off @16k) → prints `SpeechStart` then `SpeechEnd` sample positions.

### T8: qwen3_asr streaming wrapper (per utterance)

- **Files:** `src/pipeline/stt.h/.cpp`.
- **Pattern:**
  ```cpp
  class SttSession {
  public:
      struct Events { std::function<void(std::string partial)> on_partial; std::function<void(std::string final)> on_final; };
      explicit SttSession(engine::runtime::ILoadedVoiceModel& asr_model, Events ev);
      void begin_utterance();            // create session, start_stream
      void feed(const std::vector<float>& f32, int64_t start_sample); // process_audio_chunk → partials
      void end_utterance();              // finish_stream() → text_output → on_final
  };
  ```
- **Constraints:** one ASR session per utterance (created on `begin_utterance`, destroyed after `end_utterance`) — do NOT share across utterances; feed the **same 512-sample chunks** the VAD got; `finish_stream()` must be called on the pipeline thread (ISC-A-1).
- **Acceptance (ISC-5 partial):** `persona listen --stdin --streaming` on a fixture WAV pipes chunks through and prints partials + final text (script asserts final text present).

### T9: Endpointer state machine + `persona daemon` (text out)

- **Files:** `src/pipeline/endpointer.h/.cpp`, `src/daemon/daemon.cpp`, `src/protocol/ndjson.h/.cpp`, `src/main.cpp`.
- **State machine (the core product):**
  ```
  Idle ──SpeechStart──▶ Speaking ──SpeechEnd──▶ Finalizing ──finish_stream() done──▶ emit speech.final ──▶ Idle
   │                       │                         │
   │                       └── 30 s cap ────────────▶ Finalizing (force)
   └── new SpeechStart while Finalizing: buffer only (sequential, queue depth 1)
  ```
- **NDJSON out (protocol spec in plan):** `ready`, `speech.start`, `speech.partial` (throttled ≥250 ms apart), `speech.final` `{"type":"speech.final","seq":n,"text":...,"empty":bool,"duration_ms":...,"chars":...}`, `speech.error`, `shutdown`. Flush stdout after every line.
- **Daemon loop:**
  ```cpp
  // pipeline thread:
  while (running) {
      auto chunk = ring.pop_up_to(512);               // 16k mono f32
      vad.feed(chunk, start_sample);                  // events drive begin/end_utterance
      if (speaking || finalizing) stt.feed(chunk, start_sample);
  }
  // main thread: stdin reader → {"type":"stop"} → running=false → vad.finish, stt.finish, exit 0
  ```
- **Constraints:** SIGPIPE → graceful shutdown (agent closed stdout); malformed stdin line → log stderr + continue (ISC-11); all engine calls on the pipeline thread (ISC-A-1); `seq` increments per utterance.
- **Acceptance (ISC-5,6,7,9,10,12):** scripted test: play a fixture WAV into a null PA input device (or `--mic none --audio-fixture file.wav` test mode) → assert one `speech.final` per utterance; `{"type":"stop"}` → exit 0; a >30 s fixture is force-finalized; empty transcript has `"empty":true`; pipe closed → clean exit.

---

## Phase 3 — TTS + agent

### T10: Playback queue + `persona tts` verb

- **Files:** `src/audio/playback.h/.cpp`, `src/tts.cpp` (verb).
- **Pattern (output side):**
  ```cpp
  class PlaybackQueue {
  public:
      void enqueue(engine::runtime::AudioBuffer buf);   // pushes into SPSC queue of buffers
      // PA output callback pops full buffers, plays float samples at buf.sample_rate
  };
  ```
- **`persona tts`:** read text (arg or stdin), run pocket_tts offline:
  ```cpp
  auto sess = tts_model->create_task_session(
      {engine::runtime::VoiceTaskKind::Tts, engine::runtime::RunMode::Offline}, opts);
  auto* off = dynamic_cast<engine::runtime::IOfflineVoiceTaskSession*>(sess.get());
  engine::runtime::TaskRequest req; req.text_input = engine::runtime::Transcript{text, "en"};
  auto res = off->run(req);
  auto out = res.audio_output.value();  // AudioBuffer{sample_rate, channels, samples}
  ```
  `--out x.wav` writes WAV; `--play` feeds PlaybackQueue; default: `--out -` WAV to stdout (composable).
- **Reference:** pocket_tts spec `model_specs/pocket_tts.json` (English Q8_0 default), TTS usage `docs/usage.md`.
- **Acceptance (ISC-8):** `echo "hello" | persona tts --out /tmp/h.wav` writes a playable WAV (>0 bytes, valid header); `--play` audible on the HDMI device (manual).

### T11: daemon `tts` command wired

- **Files:** `src/daemon/daemon.cpp`, `src/pipeline/tts.h/.cpp`, `src/model/registry.cpp` (lazy TTS model load on first command).
- **Flow:** stdin `{"type":"tts","text":"...","seq":2}` → (a) emit `tts.start`; (b) run TTS session on **pipeline thread** (serialized with ASR — TTS is quick, acceptable; document that a long `speech.final` processing delays TTS start); (c) enqueue audio buffers to PlaybackQueue; (d) emit `tts.done` (or `tts.error` with message).
- **Constraint:** TTS session calls also only on the pipeline thread (ISC-A-1); playback thread never touches engine.
- **Acceptance (ISC-8 partial):** daemon + `echo '{"type":"tts","text":"hi"}'` → stdout shows `tts.done` with `out_ms>0`; with `--play` audio is audible.

### T12: pi RPC agent adapter — `persona daemon --agent pi` (Decision 8)

- **Files:** `src/agent/pi_rpc.h/.cpp`, `src/daemon/daemon.cpp` (wiring), `src/config.h/.cpp` (`--agent`, `--pi-args`, `--no-speak`).
- **Pattern:**
  ```cpp
  // src/agent/pi_rpc.h
  class PiAgent {
  public:
      struct Events {
          std::function<void(std::string text_delta)> on_reply_delta;      // accumulate → TTS queue
          std::function<void(std::string full)>      on_reply_complete;    // message_end authoritative
      };
      PiAgent(Events ev, std::string pi_bin = "pi", std::vector<std::string> extra_args = {});
      bool start();   // spawn `pi --mode rpc` with own stdin/stdout pipes (posix_spawn)
      void submit_utterance(int seq, const std::string& text);
      void shutdown(); // SIGTERM child, reap
  };
  ```
- **Protocol (from pi docs/rpc.md — read `/home/alexfneves/.npm-global/lib/node_modules/@earendil-works/pi-coding-agent/docs/rpc.md`):**
  - Command: `{"type":"prompt","message":"<speech.final text>","streamingBehavior":"steer"}` (steer queues while pi is mid-turn — voice-over).
  - Events: `message_update` with `assistantMessageEvent.type=="text_delta"` → accumulate per `contentIndex`; `message_end.message` is authoritative → `on_reply_complete`; ignore `thinking_*`/`toolcall_*`, `agent_*`, `turn_*`, `compaction_*`.
  - **Framing: split on `\n` only (LF-only; strip trailing `\r`); do NOT use a unicode-splitting line reader.**
  - Parse JSON with nlohmann; unknown event types are skipped, never fatal.
- **Wiring:** pi thread (reader) → `on_reply_complete` pushes a TTS request into the pipeline thread's command queue (so TTS still runs serialized on the pipeline thread, ISC-A-1); `speech.final` on the pipeline thread → `submit_utterance` (write one JSONL line to pi's stdin — `std::flush`). Emit `agent.sent` after prompt accepted (`response` with `success:true`), `agent.reply.done` after TTS enqueued (or with `spoken:false` under `--no-speak`).
- **Failure handling:** pi exits or pipe breaks → emit `agent.error`, keep daemon alive, continue NDJSON mode (logs to stderr); `{"type":"stop"}` → `shutdown()` (SIGTERM, reap) before exit 1/0.
- **Acceptance (ISC-14):** with a stub `pi` script (fake RPC responder: reads lines, echoes `{"type":"response","command":"prompt","success":true}` + one `message_update` `text_delta` + `message_end`), `persona daemon --agent pi --no-speak` in `--audio-fixture` test mode emits `agent.sent` then `agent.reply.done` with the reply chars; daemon stays up after a garbage `pi` line. Framing test: a `\r\n`-terminated event parses.

---

## Phase 4 — Polish

### T13: Error paths + config plumbing

- **Files:** `src/config.h/.cpp`, `src/main.cpp`, `src/daemon/daemon.cpp`.
- **Config (flags, no config file in v1):** `--models-root`, `--asr-family` (+`--asr-package`), `--tts-family` (+`--tts-package`), `--mic-device`, `--play-device`, `--vad-threshold` (0.5), `--vad-min-silence-ms` (100), `--vad-min-speech-ms` (250), `--utt-cap-s` (30), `--backend` (cpu), `--agent none|pi`, `--pi-args`, `--no-speak`, `--models-root`.
- **Selection validation:** `--asr-package` must exist in that family's spec (clear error + `persona models search` hint listing valid package ids); same for TTS.
- **Error surfacing:** startup model-load failure → stderr + nonzero exit with `persona models install ...` hint; runtime session exception → `speech.error` + stay up; devices verb on failure prints PA error.
- **Acceptance (ISC-9..12, 13):** all edge tests from T9 re-run green through the flag surface; `persona daemon --backend nope` fails fast with a clear message; `daemon --asr-package qwen3_asr_0_6b_q8_0 --asr-family qwen3_asr` loads (echoed in `ready`); `--asr-package` from a different family → error with hint.

### T14: README + flake check hook

- **Files:** `README.md`, `flake.nix` (`checks.${system}.smoke`), `testdata/` fixtures, `tests/daemon_smoke.sh`, `tests/pi_stub.sh`.
- **Content:** quickstart (`nix develop` → `persona models search --task tts` → `persona models install qwen3_asr pocket_tts` → `persona daemon --agent pi`), agent-side wrapper sketch (reads NDJSON, echoes `tts` back), `--agent pi` + stub-pi test note, protocol doc pointer, tuning knobs, Nix build notes (pin bump procedure).
- **Smoke hook:** `nix flake check` runs: selftest + `listen` on fixture + daemon test-mode script (T9 acceptance) + `models search --task tts` (assert pocket_tts present) + `--agent pi` stub test (T12 acceptance). Fails the build on regression.
- **Acceptance:** fresh `nix flake check` passes on a clean checkout.

---

## Sequencing notes

- T0 first (derisk). T1→T6 form Phase 1; T7→T9 Phase 2 (do not skip T7/T8 — the wrappers are the testable atoms of the daemon); T10→T12 Phase 3 (T12 needs T10 for speaking; can land with `--no-speak` first); T13→T14 last.
- **Do NOT** start T7 before T1a (artifact names) and T5 (models present) are done.
- Parallelizable: T3 ↔ T4/T5 after T1/T2; T6 can start in parallel with T7.
- If Phase 2 endpointing feels wrong on real audio, tune via T13 flags — never by editing the state machine before flagging it in the plan.