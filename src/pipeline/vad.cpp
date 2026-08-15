#include "pipeline/vad.h"

#include "backend.h"

#include "engine/framework/core/backend.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace persona {

VadSession::VadSession(engine::runtime::ILoadedVoiceModel& vad_model, Events ev)
    : model_(&vad_model), ev_(std::move(ev)) {}

void VadSession::start(std::unordered_map<std::string, std::string> vad_options,
                       const engine::core::BackendConfig& backend) {
    // Capture the start inputs for a potential restart(): a fresh session is
    // rebuilt with the SAME model, backend, and tuning options.
    start_options_ = std::move(vad_options);
    start_backend_ = backend;
    create_session();
}

void VadSession::create_session() {
    // Tear down any previous session first (restart path): the old session is
    // broken and unrecoverable — dropping it is the point.
    sess_.reset();
    stream_ = nullptr;
    speaking_ = false;

    engine::runtime::SessionOptions opts;
    // The caller's parsed --backend (not a hardcoded CPU): the Vulkan variant
    // must run VAD on the GPU like every other session (T9 review P1-1).
    opts.backend = start_backend_;
    // silero reads its config from SessionOptions.options (see
    // silero_config_from_options in src/models/silero_vad/session.cpp). The
    // TaskRequest options are only honored by the offline run() path, so
    // streaming tuning options MUST ride here. Copied (create_session may run
    // again on restart — start() moved the caller's map into start_options_).
    opts.options = start_options_;

    if (std::getenv("PERSONA_DEBUG_TIMELINE")) {
        std::cerr << "dbg: vad session backend=" << backend_name(opts.backend.type)
                  << " device=" << opts.backend.device
                  << " threads=" << opts.backend.threads << "\n";
    }

    sess_ = model_->create_task_session(
        {engine::runtime::VoiceTaskKind::Vad, engine::runtime::RunMode::Streaming}, opts);
    stream_ = dynamic_cast<engine::runtime::IStreamingVoiceTaskSession*>(sess_.get());
    if (stream_ == nullptr) {
        throw std::runtime_error("silero_vad session does not support streaming");
    }
    chunk_samples_ = stream_->streaming_policy().preferred_audio_chunk_samples;
    if (chunk_samples_ <= 0) {
        chunk_samples_ = 512;  // silero_vad's hardcoded chunk (kChunkSamples)
    }

    // The session must be prepared before start_stream(): the default
    // IStreamingVoiceTaskSession::start_stream calls reset(), which requires an
    // already-prepared session (require_prepared). An empty request leaves the
    // sample rate at the VAD's 16 kHz default.
    stream_->prepare(engine::runtime::build_preparation_request(engine::runtime::TaskRequest{}));
    stream_->start_stream({});
}

void VadSession::restart() {
    // Throws on failure; the caller keeps the degraded state (stream_ ==
    // nullptr, consecutive_failures_ at the escalation point).
    create_session();
}

void VadSession::feed(const std::vector<float>& mono_f32_16k, int64_t start_sample) {
    if (stream_ == nullptr || mono_f32_16k.empty()) {
        return;
    }
    try {
        engine::runtime::AudioChunk chunk;
        chunk.sample_rate = 16000;
        chunk.channels = 1;
        chunk.start_sample = start_sample;
        chunk.samples = mono_f32_16k;

        const engine::runtime::StreamEvent ev = stream_->process_audio_chunk(chunk);
        for (const auto& ve : ev.voice_activity) {
            switch (ve.kind) {
            case engine::runtime::VoiceActivityEvent::Kind::SpeechStart:
                if (!speaking_) {
                    speaking_ = true;
                    if (ev_.on_speech_start) {
                        ev_.on_speech_start();
                    }
                }
                break;
            case engine::runtime::VoiceActivityEvent::Kind::SpeechEnd:
                if (speaking_) {
                    speaking_ = false;
                    if (ev_.on_speech_end) {
                        ev_.on_speech_end();
                    }
                }
                break;
            case engine::runtime::VoiceActivityEvent::Kind::SpeechSegment:
                break;  // not used for endpointing
            }
        }
    } catch (const std::exception& ex) {
        // Contract: feed() never throws. Escalation: log the first few failures;
        // at 3 CONSECUTIVE failures attempt ONE restart per broken episode (the
        // episode ends on the first successful feed, which resets the counter
        // and re-arms restart). After the restart attempt, failures are logged
        // sparsely (every 10th) so a permanently broken session cannot spam the
        // log at the chunk rate (~100 lines/s). If the restart itself throws,
        // stream_ is left null and feeds become no-ops until the process exits.
        ++consecutive_failures_;
        const int n = consecutive_failures_;
        const bool was_speaking = speaking_;

        if (n <= 2) {
            std::cerr << "vad: process_audio_chunk failed: " << ex.what() << "\n";
        } else if (n == 3 && !restart_attempted_) {
            restart_attempted_ = true;
            std::cerr << "vad: session broken (" << n << " failures) — restarting\n";
            try {
                restart();
                consecutive_failures_ = 0;  // fresh session: restart the count
                std::cerr << "vad: session restarted — stream re-created\n";
            } catch (const std::exception& rex) {
                std::cerr << "vad: session restart failed: " << rex.what()
                          << " — VAD feed disabled (endpointing degraded)\n";
            }
        } else if (n % 10 == 0) {
            std::cerr << "vad: process_audio_chunk still failing (" << n
                      << " consecutive)\n";
        }

        // Keep the endpointer consistent: a failure mid-utterance closes the
        // speech out so the endpointer finalizes instead of staying in the
        // Speaking state. (After a restart the new session already starts with
        // speaking_ == false.) Note: silero's process_chunk also rejects
        // non-512-sample or non-contiguous chunks and then stays broken, so
        // callers must always feed full preferred-sized chunks.
        if (was_speaking) {
            speaking_ = false;
            if (ev_.on_speech_end) {
                ev_.on_speech_end();
            }
        }
    }
}

void VadSession::finish() {
    if (stream_ == nullptr) {
        return;
    }
    try {
        // Finalize on shutdown. silero's finalize_stream returns segments in the
        // TaskResult, not as StreamEvents, and emits no SpeechEnd of its own —
        // so any utterance left open is closed out here by hand to keep the
        // endpointer state consistent.
        (void)stream_->finish_stream();
    } catch (const std::exception& ex) {
        std::cerr << "vad: finish_stream failed: " << ex.what() << "\n";
    }
    if (speaking_) {
        speaking_ = false;
        if (ev_.on_speech_end) {
            ev_.on_speech_end();
        }
    }
    stream_ = nullptr;
    sess_.reset();
}

}  // namespace persona