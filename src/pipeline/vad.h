#pragma once

#include "engine/framework/core/backend.h"
#include "engine/framework/runtime/model.h"
#include "engine/framework/runtime/session.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace persona {

// Streaming silero_vad wrapper: a long-lived streaming VAD session that turns
// the per-chunk `voice_activity` events into one-shot start/end callbacks.
//
// Lifecycle (all calls on ONE thread — the pipeline thread, ISC-A-1):
//   VadSession v(vad_model, {on_start, on_end});
//   v.start();                                  // create session + start_stream
//   v.feed(mono_f32_16k, start_sample);         // preferred-sized chunks only
//   v.feed(...);                                // ...
//   v.finish();                                 // finish_stream() (shutdown only)
//
// feed() is non-throwing by contract: engine failures are logged to stderr and
// the internal speaking_ state is kept consistent so the endpointer can't stay
// wedged in the Speaking state on a mid-utterance exception.
//
// Failure escalation: silero's process_audio_chunk throws on non-512/non-
// contiguous chunks and the session then stays broken — events stop firing and
// endpointing silently dies. feed() counts CONSECUTIVE exceptions (reset on
// every successful feed); at 3 it attempts ONE restart() per broken episode:
// the session is destroyed and re-created from the model + options + backend
// captured at start(), speaking_ resets, and chunk_samples_ is re-read from
// the fresh session. The episode ends on the first successful feed (which
// re-arms restart). If the restart itself throws, the session stays broken
// (stream_ == nullptr — subsequent feeds are no-ops) and failures are logged
// sparsely thereafter; the daemon surfaces the degraded state via
// consecutive_failures().
class VadSession {
public:
    struct Events {
        std::function<void()> on_speech_start;
        std::function<void()> on_speech_end;
    };

    explicit VadSession(engine::runtime::ILoadedVoiceModel& vad_model, Events ev);

    // Creates the streaming Vad session and starts the stream. `vad_options`
    // are silero tuning options (e.g. {"threshold","0.5"},
    // {"min_speech_duration_ms","250"}); they are merged into
    // SessionOptions.options (NOT the TaskRequest — the streaming path only
    // reads them from the session options). `backend` selects the compute
    // backend for the session (the daemon passes its parsed --backend; the
    // selftest leaves the CPU default). Throws on setup failure; call on the
    // pipeline thread.
    void start(std::unordered_map<std::string, std::string> vad_options = {},
               const engine::core::BackendConfig& backend = engine::core::BackendConfig{});

    // Feeds one chunk of 16 kHz mono f32 audio at the given absolute sample
    // offset. Chunks MUST be `chunk_samples()` large and contiguous (silero
    // throws otherwise). Fires on_speech_start / on_speech_end exactly once
    // per speaking transition. Never throws.
    void feed(const std::vector<float>& mono_f32_16k, int64_t start_sample);

    // Finalizes the stream; call once on daemon shutdown after the last feed().
    // Emits a closing on_speech_end if an utterance was left open. Never throws.
    void finish();

    // Preferred chunk size in samples reported by streaming_policy()
    // (silero_vad = 512). The daemon chunks by this policy.
    int64_t chunk_samples() const noexcept { return chunk_samples_; }

    // Consecutive feed() exceptions since the last successful feed (0 when
    // healthy). The daemon reads this to log a higher-level "endpointing
    // degraded" message once per broken episode (>= 3 = escalated / broken).
    int consecutive_failures() const noexcept { return consecutive_failures_; }

private:
    // Creates a fresh streaming session from the model + options + backend
    // captured at start(). Destroys any previous session first (restart
    // path). Throws on failure — stream_ is left null, so subsequent feeds
    // are no-ops (the session stays broken until the process is restarted).
    void create_session();
    // Recovers a broken session: destroy + recreate (driven by feed()'s
    // failure counter, at most once per broken episode). Throws on failure.
    void restart();

    engine::runtime::ILoadedVoiceModel* model_;
    std::unique_ptr<engine::runtime::IVoiceTaskSession> sess_;
    engine::runtime::IStreamingVoiceTaskSession* stream_ = nullptr;
    Events ev_;
    bool speaking_ = false;
    int64_t chunk_samples_ = 0;
    // Restart state: start()'s inputs are captured so restart() can rebuild
    // the session identically (same model, backend, tuning options).
    std::unordered_map<std::string, std::string> start_options_;
    engine::core::BackendConfig start_backend_;
    int consecutive_failures_ = 0;    // feed() exceptions since last success
    bool restart_attempted_ = false;  // one restart per broken episode
};

}  // namespace persona