#pragma once

#include "engine/framework/core/backend.h"
#include "engine/framework/runtime/model.h"
#include "engine/framework/runtime/session.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace persona {

// Streaming ASR wrapper (qwen3_asr): one session per utterance.
//
// Lifecycle (all calls on ONE thread — the pipeline thread, ISC-A-1):
//   SttSession stt(asr_model, {on_partial, on_final, on_error});
//   stt.begin_utterance();                  // fresh session + start_stream
//   stt.feed(mono_f32_16k, start_sample);   // any chunk size (qwen3 buffers internally)
//   stt.feed(...);                          // partials fire as windows complete
//   stt.end_utterance();                    // finish_stream() -> on_final
//
// A session is created in begin_utterance() and destroyed in
// end_utterance()/abort() — never reused across utterances.
//
// feed() is non-throwing by contract (same as VadSession): an engine
// exception is logged, surfaced through on_error, and the session is marked
// broken so end_utterance() skips finish_stream() and never hangs.
//
// qwen3_asr specifics (verified against release-0.6 session.cpp):
//   * Streaming output is StreamingOutputKind::FinalResult — process_audio_chunk
//     returns the StreamEvent synchronously (no PullEvents).
//   * StreamEvent.partial_text (std::optional<Transcript>) carries the DELTA
//     since the last partial (substr of the cumulative streaming text), so we
//     accumulate deltas here and fire on_partial with the cumulative text —
//     which is what the daemon protocol wants.
//   * The default streaming window is 30 s (kDefaultStreamingWindowSeconds), so
//     without audio_chunk_seconds set in the start_stream request options no
//     partial would ever fire on a short utterance. We set 1.0 s — the model's
//     preferred feed chunk (0.5 s hallucinates: each inference sees too little
//     context and the transcript degrades; see stt.cpp) — matching what the
//     offline CLI uses.
class SttSession {
public:
    struct Events {
        std::function<void(std::string partial)> on_partial;
        std::function<void(std::string final)> on_final;
        // Engine failures during feed()/end_utterance() surface here (the
        // daemon maps this to speech.error). Not fired for user-level aborts.
        std::function<void(std::string err)> on_error;
    };

    explicit SttSession(engine::runtime::ILoadedVoiceModel& asr_model, Events ev);

    // Creates a fresh streaming Asr session and starts the stream. `backend`
    // selects the compute backend (the daemon/listen pass their parsed
    // --backend; the Vulkan variant must run ASR on the GPU — T9 review
    // P1-1). If a session is already live (defensive), logs a warning and ends
    // it first. Throws on setup failure; call on the pipeline thread.
    void begin_utterance(const engine::core::BackendConfig& backend);

    // Pre-creates the next utterance's session (create_task_session +
    // prepare) while the pipeline is IDLE, so begin_utterance() later only
    // pays start_stream (~ms) instead of the full ~0.7 s session init on the
    // critical path (measured fix for dropped utterance onsets). The session
    // is still fresh per utterance — never reused. Idempotent: no-op when a
    // prepared session already exists; no-op (with a warning) while live.
    // Throws on setup failure; call on the pipeline thread.
    void prepare(const engine::core::BackendConfig& backend);

    // Feeds one chunk of 16 kHz mono f32 audio at the given absolute sample
    // offset. qwen3 buffers internally and accepts any chunk size, but the
    // daemon feeds the same 512-sample chunks the VAD got. Fires on_partial
    // with the cumulative transcript as 1.0 s windows complete. Never throws.
    void feed(const std::vector<float>& mono_f32_16k, int64_t start_sample);

    // Finalizes the utterance: finish_stream() -> text_output -> on_final.
    // Always completes (never blocks forever); if feed() previously errored,
    // the broken session is destroyed without calling finish_stream(). Never
    // throws.
    void end_utterance();

    // Best-effort discard (barge-in path for T9): finishes the stream and
    // throws the result away — no on_final. If the session is already broken,
    // just destroys it. Never throws.
    void abort();

    // True while an utterance session is live (between begin/end).
    bool live() const noexcept { return live_; }

private:
    engine::runtime::ILoadedVoiceModel* model_;
    std::unique_ptr<engine::runtime::IVoiceTaskSession> sess_;
    engine::runtime::IStreamingVoiceTaskSession* stream_ = nullptr;
    Events ev_;
    std::string running_partial_;
    bool live_ = false;
    bool prepared_ = false;  // sess_ exists, prepared, NOT started
    bool broken_ = false;

    // Destroys any existing session and creates + prepares a fresh one
    // (shared by prepare() and begin_utterance()'s fallback). Throws on
    // failure.
    void create_session(const engine::core::BackendConfig& backend);
};

}  // namespace persona
