#include "pipeline/tts.h"

#include "engine/framework/runtime/session.h"

#include <algorithm>
#include <stdexcept>
#include <thread>
#include <utility>

namespace persona {

TtsSession::Result TtsSession::run(engine::runtime::ILoadedVoiceModel& tts_model,
                                   const std::string& text,
                                   const engine::core::BackendConfig& backend) {
    Result out;
    try {
        if (text.empty()) {
            out.error = "empty text";
            return out;
        }

        engine::runtime::SessionOptions opts;
        opts.backend = backend;

        auto sess = tts_model.create_task_session(
            {engine::runtime::VoiceTaskKind::Tts, engine::runtime::RunMode::Offline},
            opts);
        auto* off = dynamic_cast<engine::runtime::IOfflineVoiceTaskSession*>(sess.get());
        if (off == nullptr) {
            out.error = "tts session does not support offline execution";
            return out;
        }

        engine::runtime::TaskRequest req;
        req.text_input = engine::runtime::Transcript{text, "en"};
        // PocketTTS requires a voice selection (prepare() throws otherwise).
        // The english q8_0 package ships exactly one speaker embedding
        // (embeddings/alba.safetensors), so "alba" is the default voice.
        engine::runtime::VoiceCondition voice;
        engine::runtime::VoiceReference ref;
        ref.cached_voice_id = "alba";
        voice.speaker = std::move(ref);
        req.voice = std::move(voice);
        // Pin the RNG seed so synthesis is deterministic: `persona tts` to a
        // file and `persona tts | ...` to stdout must produce identical output
        // (the engine defaults to a random seed per run). A T13 --tts-* knob
        // can expose it.
        req.options["seed"] = "0";
        // prepare() is mandatory before run() (PocketTTSSession::run calls
        // require_prepared). build_preparation_request carries text_input over.
        sess->prepare(engine::runtime::build_preparation_request(req));
        const engine::runtime::TaskResult res = off->run(req);

        if (!res.audio_output) {
            out.error = "model returned no audio output";
            return out;
        }
        const engine::runtime::AudioBuffer& audio = *res.audio_output;
        out.sample_rate = audio.sample_rate;
        if (audio.channels <= 1) {
            out.samples = audio.samples;
        } else {
            // Downmix interleaved channels to mono (the playback sink is mono;
            // pocket_tts itself outputs mono, this guards other TTS families).
            const size_t frames = audio.samples.size() /
                                  static_cast<size_t>(audio.channels);
            out.samples.reserve(frames);
            for (size_t f = 0; f < frames; ++f) {
                float acc = 0.0f;
                for (int c = 0; c < audio.channels; ++c) {
                    acc += audio.samples[f * static_cast<size_t>(audio.channels) +
                                         static_cast<size_t>(c)];
                }
                out.samples.push_back(acc / static_cast<float>(audio.channels));
            }
        }
        out.ok = true;
    } catch (const std::exception& ex) {
        out.error = ex.what();
    }
    return out;
}

}  // namespace persona
