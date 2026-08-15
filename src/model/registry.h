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
    std::unique_ptr<engine::runtime::ILoadedVoiceModel> tts_model;
};

// Builds the runtime and loads silero_vad from the bundled assets (required —
// throws on failure) plus the ASR (qwen3_asr) and TTS (pocket_tts) models from
// the models root (soft failure: a missing model is left null for now; the
// verb/daemon decide how to surface it). Eager TTS load keeps one code path
// for the tts verb; T11 can switch to lazy if daemon startup cost matters.
// Call on the pipeline thread only.
Runtime make_runtime(const Config& cfg);

}  // namespace persona