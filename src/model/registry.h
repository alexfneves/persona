#pragma once

#include "config.h"

#include "engine/framework/runtime/registry.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace persona {

// One resolved model selection (T13): which family + package id the runtime
// should load, and where its files live under the models root.
struct ModelSelection {
    std::string family;      // e.g. "qwen3_asr"
    std::string package_id;  // e.g. "qwen3_asr_1_7b_q8_0"
    // Every file of the package, relative to models_root after strip_prefix
    // (install targets — see package_file_target). Used to check the package
    // is actually installed before loading.
    std::vector<std::string> files;
    // The model directory under the models root (the package's
    // target_directory, or the family name when the spec has none).
    std::filesystem::path target_dir;
};

// Resolves which package of `family` to load from the shipped catalog.
// package_id empty -> the spec's default package ("default":true, else the
// first). `task` is the required capability ("asr" or "tts"). THROWS
// std::runtime_error with a user-facing hint — fail fast, T13 — when:
//   * the family is unknown            (hint: persona models search --task <t> --q <f>)
//   * the family has no <task> task    (hint: persona models search --task <t>)
//   * the package id is not in the spec (hint: persona models info <family> + valid ids)
// The DIR existence is NOT checked here — a missing install is a soft-fail
// load (null model + install hint surfaced by the verb/daemon), not a config
// error.
ModelSelection resolve_model_selection(const Config& cfg,
                                       const std::string& family,
                                       const std::string& package_id,
                                       const std::string& task);

// Builds the "install it with: persona models install ..." hint for a missing
// model, echoing the requested family (and package when one was requested).
std::string install_hint(const std::string& family, const std::string& package);

// Why a model did not load — the three modes have different user-facing
// fixes, and the composite build (-DAUDIOCPP_MODELS in flake.nix) only
// compiles a subset of the engine's loaders, so a family that IS on disk may
// still be unloadable in this binary (T15).
enum class LoadFail : int {
    None,          // loaded (or no load attempted)
    NotInstalled,  // package files missing under the models root
    LoaderMissing, // family has no loader compiled into this binary
    EngineError,   // registry.load threw (broken/corrupt install)
};

// The engine runtime plus the models loaded at startup.
struct Runtime {
    engine::runtime::ModelRegistry registry = engine::runtime::make_default_registry();
    std::unique_ptr<engine::runtime::ILoadedVoiceModel> vad_model;
    std::unique_ptr<engine::runtime::ILoadedVoiceModel> asr_model;
    std::unique_ptr<engine::runtime::ILoadedVoiceModel> tts_model;
    // Resolved model selection (T13): the family + package id that were
    // resolved for ASR/TTS — the daemon echoes them in the ready line (e.g.
    // "asr_package":"qwen3_asr_1_7b_q8_0") even when the model is not
    // installed (so the agent knows what WOULD load).
    std::string asr_family;
    std::string asr_package;
    std::string tts_family;
    std::string tts_package;
    // Compiled-in loader family names (advertise_loaders) — the reference set
    // for "can this binary load family X" (T15). Listed verbatim in the
    // LoaderMissing hint so the user sees what this binary CAN load.
    std::vector<std::string> loader_families;
    // Why the ASR/TTS model did not load (None when loaded). The surfacers
    // pick the hint: install vs rebuild vs reinstall.
    LoadFail asr_load_fail = LoadFail::None;
    std::string asr_load_fail_detail;  // EngineError only: ex.what()
    LoadFail tts_load_fail = LoadFail::None;
    std::string tts_load_fail_detail;  // EngineError only: ex.what()
};

// The actionable hint for a failed model load — one line, no trailing
// newline, matching the existing "  install it with: ..." block style.
// Distinguishes the three LoadFail modes:
//   NotInstalled  -> "  install it with:  persona models install <family>..."
//   LoaderMissing -> "  family '<family>' is not compiled into this binary
//                    (available loaders: ...); add it to AUDIOCPP_MODELS in
//                    flake.nix and rebuild (nix build .#persona or
//                    .#persona-vulkan)"
//   EngineError   -> "  reinstall it with:  persona models install <family>..."
//                    (the engine's message is already logged by try_load).
std::string load_failure_hint(const Runtime& rt, LoadFail fail,
                              const std::string& family,
                              const std::string& package,
                              const std::string& detail);

// Builds the runtime and loads silero_vad from the bundled assets (required —
// throws on failure) plus the ASR (qwen3_asr by default) and TTS (pocket_tts)
// models from the models root. The family/package selection is validated
// against the catalog FIRST (throws with a hint — fail fast); a missing model
// dir is a SOFT failure (null model; the verb/daemon surface the install
// hint). Eager TTS load keeps one code path for the tts verb; T11 can switch
// to lazy if daemon startup cost matters. Call on the pipeline thread only.
Runtime make_runtime(const Config& cfg);

}  // namespace persona