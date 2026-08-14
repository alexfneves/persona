#pragma once

#include "config.h"

#include "engine/framework/runtime/registry.h"

#include <memory>

namespace persona {

// The engine runtime plus the models loaded at startup.
struct Runtime {
    engine::runtime::ModelRegistry registry = engine::runtime::make_default_registry();
    std::unique_ptr<engine::runtime::ILoadedVoiceModel> vad_model;
    std::unique_ptr<engine::runtime::ILoadedVoiceModel> asr_model;
};

// Builds the runtime and loads silero_vad from the bundled assets (required —
// throws on failure) plus the ASR model from the models root (soft failure:
// a missing ASR model is left null for now; the daemon decides how to surface
// it). Call on the pipeline thread only.
Runtime make_runtime(const Config& cfg);

}  // namespace persona