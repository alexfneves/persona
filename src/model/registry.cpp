#include "model/registry.h"

#include "engine/framework/runtime/model.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace persona {

namespace fs = std::filesystem;

namespace {

// Directory containing the bundled silero_vad model
// (assets/framework/models/silero_vad). Resolution order, mirroring specs_dir:
//   1. $PERSONA_ASSETS_DIR env (dev/test override)
//   2. relative to the real executable: <exe>/../assets/framework/models/silero_vad
//      (/proc/self/exe so `result/bin/persona` symlinks and PATH invocations
//      resolve to the actual store path)
//   3. relative fallback
fs::path resolve_vad_assets_dir() {
    if (const char* env = std::getenv("PERSONA_ASSETS_DIR"); env != nullptr && *env != '\0') {
        return fs::path(env);
    }
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        return (exe.parent_path() / ".." / "assets" / "framework" / "models" / "silero_vad")
            .lexically_normal();
    }
    return "assets/framework/models/silero_vad";
}

// Family-specific model directory under the models root, resolved from the
// shipped spec catalog: the default package's `target_directory` (e.g. the
// qwen3_asr spec marks Qwen3-ASR-1.7B-GGUF as default; pocket_tts marks
// PocketTTS-GGUF/english). Falls back to the plain family name if the spec
// cannot be read — the catalog machinery (T4) made this lookup rigorous.
fs::path resolve_model_dir(const Config& cfg, const std::string& family) {
    const fs::path base(cfg.models_root);
    try {
        const fs::path spec_path = fs::path(cfg.specs_dir) / (family + ".json");
        std::ifstream in(spec_path);
        if (in) {
            nlohmann::json spec;
            in >> spec;
            for (const auto& pkg : spec.at("packages")) {
                if (pkg.value("default", false)) {
                    const std::string dir = pkg.value("target_directory", "");
                    if (!dir.empty()) {
                        return base / dir;
                    }
                }
            }
            // No package flagged default: use the first package's directory.
            if (!spec.at("packages").empty()) {
                const std::string dir =
                    spec.at("packages")[0].value("target_directory", "");
                if (!dir.empty()) {
                    return base / dir;
                }
            }
        }
    } catch (const std::exception&) {
        // Fall through to the family-name default below.
    }
    return base / family;
}

}  // namespace

Runtime make_runtime(const Config& cfg) {
    Runtime rt;

    // Required: bundled silero_vad (ships in $out/assets). Failure is fatal.
    engine::runtime::ModelLoadRequest vad_request;
    vad_request.model_path = resolve_vad_assets_dir();
    vad_request.family_hint = "silero_vad";
    rt.vad_model = rt.registry.load(vad_request);

    // ASR model from the models root (spec-resolved family dir). Soft failure —
    // a missing model just leaves asr_model null; listen() and the daemon (T9)
    // decide how to surface it. registry.load throws eagerly on a missing path,
    // so a not-yet-installed model is a caught no-op here.
    try {
        engine::runtime::ModelLoadRequest asr_request;
        asr_request.model_path = resolve_model_dir(cfg, "qwen3_asr");
        asr_request.family_hint = "qwen3_asr";
        rt.asr_model = rt.registry.load(asr_request);
    } catch (const std::exception&) {
        // Soft error: selftest reports it as info, daemon handles it later.
    }

    // TTS model (pocket_tts), same spec-resolved lookup + soft failure. The
    // tts verb surfaces the missing-model hint; the daemon will in T11.
    try {
        engine::runtime::ModelLoadRequest tts_request;
        tts_request.model_path = resolve_model_dir(cfg, "pocket_tts");
        tts_request.family_hint = "pocket_tts";
        rt.tts_model = rt.registry.load(tts_request);
    } catch (const std::exception&) {
        // Soft error.
    }

    return rt;
}

}  // namespace persona