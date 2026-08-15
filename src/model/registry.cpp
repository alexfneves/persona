#include "model/registry.h"

#include "engine/framework/runtime/model.h"
#include "model/catalog.h"
#include "model/download.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

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

// True when every file the package would install exists under models_root.
// The engine's model-dir loader (require_selected_source) loads WHATEVER
// single GGUF is in the target dir, so without this check --asr-package
// qwen3_asr_1_7b_f16 on a dir holding only the q8_0 gguf would silently load
// the wrong variant. A missing file means "this package is not installed" ->
// the caller surfaces the install hint and exits nonzero (T13).
bool package_files_installed(const fs::path& models_root,
                             const ModelSelection& sel) {
    for (const std::string& rel : sel.files) {
        std::error_code ec;
        if (!fs::is_regular_file(models_root / rel, ec)) {
            return false;
        }
    }
    return true;
}

// Loads one model (family_hint) from `sel.target_dir`, returning null on a
// missing/broken install (a soft fail the caller surfaces as an install hint).
std::unique_ptr<engine::runtime::ILoadedVoiceModel> try_load(
    engine::runtime::ModelRegistry& registry, const ModelSelection& sel,
    const fs::path& models_root) {
    if (!package_files_installed(models_root, sel)) {
        return nullptr;
    }
    try {
        engine::runtime::ModelLoadRequest req;
        req.model_path = sel.target_dir;
        req.family_hint = sel.family;
        return registry.load(req);
    } catch (const std::exception& ex) {
        // Broken install / unsupported model — soft fail (caller surfaces the
        // install hint), but SURFACE the engine's error so a corrupt-but-
        // installed model isn't misreported as "not installed" (review P2).
        std::cerr << "registry: load failed for " << sel.target_dir << " ("
                  << sel.family << "): " << ex.what() << "\n";
        return nullptr;
    }
}

}  // namespace

ModelSelection resolve_model_selection(const Config& cfg,
                                       const std::string& family,
                                       const std::string& package_id,
                                       const std::string& task) {
    const std::vector<Spec> specs = load_catalog(cfg.specs_dir);
    const Spec* spec = find_spec(specs, family);
    if (spec == nullptr) {
        if (specs.empty()) {
            // An empty catalog is a different (environmental) failure than an
            // unknown family: name the specs dir so the user can fix
            // --specs-dir / PERSONA_SPECS_DIR instead of chasing a phantom
            // family.
            throw std::runtime_error(
                "model catalog is empty/unreadable at '" + cfg.specs_dir +
                "' — cannot validate model selection for family '" + family +
                "'");
        }
        throw std::runtime_error(
            "unknown model family '" + family + "' (expected a " + task +
            " family)\n  try: persona models search --task " + task + " --q " +
            family);
    }
    if (!task.empty()) {
        const bool has_task =
            std::find(spec->tasks.begin(), spec->tasks.end(), task) !=
            spec->tasks.end();
        if (!has_task) {
            throw std::runtime_error(
                "model family '" + family + "' does not support the " + task +
                " task\n  try: persona models search --task " + task);
        }
    }

    // Package selection: an explicit --*-package must exist in this family's
    // spec; otherwise use the spec's default ("default":true, else the first).
    const Package* pkg = nullptr;
    if (!package_id.empty()) {
        for (const Package& p : spec->packages) {
            if (p.id == package_id) {
                pkg = &p;
                break;
            }
        }
        if (pkg == nullptr) {
            std::string valid;
            for (size_t i = 0; i < spec->packages.size(); ++i) {
                if (i > 0) {
                    valid += ", ";
                }
                valid += spec->packages[i].id;
            }
            throw std::runtime_error(
                "unknown package '" + package_id + "' for family '" + family +
                "'\n  valid package ids: " + valid +
                "\n  try: persona models info " + family);
        }
    } else {
        for (const Package& p : spec->packages) {
            if (p.is_default) {
                pkg = &p;
                break;
            }
        }
        if (pkg == nullptr && !spec->packages.empty()) {
            pkg = &spec->packages[0];
        }
        if (pkg == nullptr) {
            throw std::runtime_error(
                "model family '" + family + "' has no packages");
        }
    }

    ModelSelection sel;
    sel.family = family;
    sel.package_id = pkg->id;
    sel.files.reserve(pkg->files.size());
    for (const std::string& f : pkg->files) {
        sel.files.push_back(package_file_target(*pkg, f).string());
    }
    if (!pkg->target_directory.empty()) {
        sel.target_dir = fs::path(cfg.models_root) / pkg->target_directory;
    } else {
        sel.target_dir = fs::path(cfg.models_root) / family;
    }
    return sel;
}

std::string install_hint(const std::string& family, const std::string& package) {
    std::string cmd = "persona models install " + family;
    if (!package.empty()) {
        cmd += " --package " + package;
    }
    return cmd;
}

Runtime make_runtime(const Config& cfg) {
    Runtime rt;

    // Required: bundled silero_vad (ships in $out/assets). Failure is fatal.
    engine::runtime::ModelLoadRequest vad_request;
    vad_request.model_path = resolve_vad_assets_dir();
    vad_request.family_hint = "silero_vad";
    rt.vad_model = rt.registry.load(vad_request);

    // ASR + TTS selection: resolve against the catalog FIRST. Bad family /
    // package ids are CONFIG errors — resolve_model_selection throws (fail
    // fast; main() turns it into a stderr message + exit 1). A missing model
    // dir is an INSTALL-STATE error: try_load returns null and the verb/daemon
    // surface the install hint. The resolved ids ride on the Runtime so the
    // daemon can echo them in ready even when the model is not installed.
    const ModelSelection asr =
        resolve_model_selection(cfg, cfg.asr_family, cfg.asr_package, "asr");
    rt.asr_family = asr.family;
    rt.asr_package = asr.package_id;
    rt.asr_model = try_load(rt.registry, asr, cfg.models_root);

    const ModelSelection tts =
        resolve_model_selection(cfg, cfg.tts_family, cfg.tts_package, "tts");
    rt.tts_family = tts.family;
    rt.tts_package = tts.package_id;
    rt.tts_model = try_load(rt.registry, tts, cfg.models_root);

    return rt;
}

}  // namespace persona
