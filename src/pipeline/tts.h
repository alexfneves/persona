#pragma once

#include "engine/framework/core/backend.h"
#include "engine/framework/runtime/model.h"

#include <string>
#include <vector>

namespace persona {

// Offline TTS runner (T10): one-shot text -> mono float audio synthesis over a
// single loaded TTS model (pocket_tts). All engine calls happen on the
// caller's thread — the tts verb's main thread today, the daemon pipeline
// thread in T11 (ISC-A-1).
//
// Non-throwing by contract: failures are captured in Result::ok/error so the
// daemon can map them to tts.error without a try/catch in the pipeline loop.
class TtsSession {
public:
    struct Result {
        bool ok = false;
        std::string error;      // populated when !ok
        int sample_rate = 0;    // native rate of samples (pocket_tts: 24 kHz)
        std::vector<float> samples;  // mono
    };

    // Synthesizes `text` with `tts_model` on `backend`. The result's samples
    // are mono (multichannel engine output is downmixed — the playback sink is
    // mono). Guarded: a missing audio_output surfaces as !ok, never a crash.
    // Never throws.
    static Result run(engine::runtime::ILoadedVoiceModel& tts_model,
                      const std::string& text,
                      const engine::core::BackendConfig& backend);
};

}  // namespace persona
