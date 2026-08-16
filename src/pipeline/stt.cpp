#include "pipeline/stt.h"

#include "backend.h"

#include "engine/framework/core/backend.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace persona {

// qwen3_asr only emits partials as its internal streaming window completes
// (default 30 s). 1.0 s matches the model's preferred_audio_chunk_seconds and
// is the smallest window that still transcribes short utterances correctly —
// at 0.5 s windows each inference sees too little context and the transcript
// degrades to hallucinated fragments (verified empirically: 0.5 s ->
// "Hello. World. letra cena é", 1.0 s+ -> "Hello, world. This is a test.").
// A T13 config candidate (--asr-window-secs).
constexpr const char* kStreamWindowSeconds = "1.0";

SttSession::SttSession(engine::runtime::ILoadedVoiceModel& asr_model, Events ev)
    : model_(&asr_model), ev_(std::move(ev)) {}

void SttSession::create_session(const engine::core::BackendConfig& backend) {
    // A fresh session per utterance (never reused): any existing session is
    // destroyed first (e.g. a leftover from a failed begin).
    sess_.reset();
    stream_ = nullptr;
    prepared_ = false;

    engine::runtime::SessionOptions opts;
    // The caller's parsed --backend (not a hardcoded CPU): the Vulkan variant
    // must run ASR on the GPU like every other session (T9 review P1-1).
    opts.backend = backend;

    if (std::getenv("PERSONA_DEBUG_TIMELINE")) {
        std::cerr << "dbg: asr session backend=" << backend_name(opts.backend.type)
                  << " device=" << opts.backend.device
                  << " threads=" << opts.backend.threads << "\n";
    }

    sess_ = model_->create_task_session(
        {engine::runtime::VoiceTaskKind::Asr, engine::runtime::RunMode::Streaming}, opts);
    stream_ = dynamic_cast<engine::runtime::IStreamingVoiceTaskSession*>(sess_.get());
    if (stream_ == nullptr) {
        sess_.reset();
        throw std::runtime_error("qwen3_asr session does not support streaming");
    }

    // prepare() is mandatory before start_stream(). Unlike silero_vad, qwen3
    // REQUIRES an audio contract in the preparation request (throws otherwise);
    // max_input_samples = 0 leaves the window unbounded, sample rate 16 kHz.
    engine::runtime::SessionPreparationRequest prep;
    prep.audio = engine::runtime::AudioPreparationContract{16000, 1, 0};
    stream_->prepare(prep);
}

void SttSession::prepare(const engine::core::BackendConfig& backend) {
    if (live_) {
        std::cerr << "stt: prepare while a session is live — ignoring (begin it first)\n";
        return;
    }
    if (sess_ && prepared_) {
        return;  // already ready for the next utterance
    }
    create_session(backend);
    prepared_ = true;
}

void SttSession::begin_utterance(const engine::core::BackendConfig& backend) {
    // Defensive: never stack sessions. A stale live session is ended first.
    if (live_) {
        std::cerr << "stt: begin_utterance while a session is live — ending it first\n";
        end_utterance();
    }
    // Use the pre-created session (fast path); fall back to a full create if
    // none was prepared (e.g. first utterance after daemon start, or after an
    // aborted begin).
    if (!sess_ || !prepared_) {
        create_session(backend);
    }

    // audio_chunk_seconds rides the start_stream TaskRequest options (qwen3
    // reads it from streaming_request_.options), NOT SessionOptions.options.
    engine::runtime::TaskRequest req;
    req.options["audio_chunk_seconds"] = kStreamWindowSeconds;
    stream_->start_stream(req);

    running_partial_.clear();
    session_pos_ = 0;
    broken_ = false;
    prepared_ = false;
    live_ = true;
}

void SttSession::feed(const std::vector<float>& mono_f32_16k,
                         [[maybe_unused]] int64_t start_sample) {
    if (stream_ == nullptr || !live_ || broken_ || mono_f32_16k.empty()) {
        return;
    }
    try {
        engine::runtime::AudioChunk chunk;
        chunk.sample_rate = 16000;
        chunk.channels = 1;
        // Session-local 0-based contiguous position (see stt.h); the caller's
        // global absolute start_sample is intentionally ignored.
        chunk.start_sample = session_pos_;
        chunk.samples = mono_f32_16k;

        const engine::runtime::StreamEvent ev = stream_->process_audio_chunk(chunk);
        session_pos_ += static_cast<int64_t>(mono_f32_16k.size());
        if (ev.partial_text && !ev.partial_text->text.empty()) {
            // qwen3 emits buffered transcript DELTAS (substr since the last
            // published partial); accumulate into the running transcript and
            // fire the cumulative text — what the daemon protocol wants.
            running_partial_ += ev.partial_text->text;
            if (ev_.on_partial) {
                ev_.on_partial(running_partial_);
            }
        }
    } catch (const std::exception& ex) {
        // Contract: feed() never throws. Log, surface through on_error, and
        // mark the session broken so end_utterance() destroys it without
        // calling finish_stream() (which would hang/throw on a wedged session).
        std::cerr << "stt: process_audio_chunk failed: " << ex.what() << "\n";
        broken_ = true;
        if (ev_.on_error) {
            ev_.on_error(ex.what());
        }
    }
}

void SttSession::end_utterance() {
    if (stream_ == nullptr || !live_) {
        return;
    }
    if (!broken_) {
        try {
            const engine::runtime::TaskResult res = stream_->finish_stream();
            if (ev_.on_final) {
                ev_.on_final(res.text_output ? res.text_output->text : std::string());
            }
        } catch (const std::exception& ex) {
            // finish_stream must always complete; on failure surface the error
            // instead of hanging or propagating.
            std::cerr << "stt: finish_stream failed: " << ex.what() << "\n";
            if (ev_.on_error) {
                ev_.on_error(ex.what());
            }
        }
    }
    // Destroy the session (never reused across utterances); the daemon
    // pre-creates the next one via prepare() while idle.
    stream_ = nullptr;
    sess_.reset();
    live_ = false;
    broken_ = false;
    prepared_ = false;
}

void SttSession::abort() {
    if (stream_ == nullptr || !live_) {
        return;
    }
    if (!broken_) {
        try {
            // Best-effort finish; the transcript is deliberately discarded
            // (barge-in path for T9 — no on_final for an aborted utterance).
            (void)stream_->finish_stream();
        } catch (const std::exception& ex) {
            std::cerr << "stt: abort finish_stream failed: " << ex.what() << "\n";
        }
    }
    stream_ = nullptr;
    sess_.reset();
    live_ = false;
    broken_ = false;
    prepared_ = false;
}

}  // namespace persona
