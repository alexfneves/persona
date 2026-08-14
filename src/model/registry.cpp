#include "model/registry.h"

#include "engine/framework/runtime/model.h"

#include <cstdlib>
#include <filesystem>
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

}  // namespace

Runtime make_runtime(const Config& cfg) {
    Runtime rt;

    // Required: bundled silero_vad (ships in $out/assets). Failure is fatal.
    engine::runtime::ModelLoadRequest vad_request;
    vad_request.model_path = resolve_vad_assets_dir();
    vad_request.family_hint = "silero_vad";
    rt.vad_model = rt.registry.load(vad_request);

    // Optional for now: ASR model from the models root. Soft failure — a
    // missing model just leaves asr_model null; the daemon (T9) decides how to
    // surface it. Note: family_hint pins the loader; the load path is the whole
    // models root until --asr-family/--asr-package selection lands (T13).
    try {
        engine::runtime::ModelLoadRequest asr_request;
        asr_request.model_path = fs::path(cfg.models_root);
        asr_request.family_hint = "qwen3_asr";
        rt.asr_model = rt.registry.load(asr_request);
    } catch (const std::exception&) {
        // Soft error: selftest reports it as info, daemon handles it later.
    }

    return rt;
}

}  // namespace persona