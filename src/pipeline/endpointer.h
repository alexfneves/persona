#pragma once

// Endpointer state machine (T9 — the core product): turns silero_vad
// SpeechStart/SpeechEnd events plus a hard utterance cap into per-utterance
// ASR lifecycle intents that the daemon loop resolves into SttSession calls.
//
//   Idle ──SpeechStart──▶ Speaking ──SpeechEnd──▶ Finalizing ──finalize done──▶ Idle
//    │                       │                        │
//    │                       └── 30 s cap ──────────▶ Finalizing (force)
//    └── SpeechStart while Finalizing: pending_ (barge-in, queue depth 1)
//
// The machine deals ONLY in absolute 16 kHz sample positions; the daemon
// converts samples <-> ms. All calls happen on ONE thread (the pipeline
// thread, ISC-A-1).
//
// Wiring (documented in todos.md T9): the daemon loop feeds each 512-sample
// chunk to VadSession first; its callbacks fire synchronously and call
// on_vad_start/on_vad_end with the chunk's start sample. The loop then calls
// tick(chunk_end) for the cap, feeds the chunk to SttSession while the
// machine is audio-active, and finally drains intents with next_intent():
//   BeginUtterance -> stt.begin_utterance() (+ emit speech.start)
//   EndUtterance   -> stt.end_utterance()   (+ emit speech.final)

#include <cstdint>
#include <deque>

namespace persona {

class Endpointer {
public:
    enum class State { Idle, Speaking, Finalizing };
    enum class Intent { None, BeginUtterance, EndUtterance };

    explicit Endpointer(int64_t cap_samples) : cap_samples_(cap_samples) {}

    // ---- fed by the daemon loop (from VadSession callbacks / per chunk) ----

    // A VAD SpeechStart fired at `sample` (the chunk start being fed). Idle ->
    // Speaking (queues BeginUtterance); Speaking -> ignored (defensive —
    // VadSession already dedupes by its internal speaking_ flag); Finalizing ->
    // barge-in: buffered only (queue depth 1), reopened the moment the current
    // finalize completes.
    void on_vad_start(int64_t sample);

    // A VAD SpeechEnd fired at `sample`. Speaking -> Finalizing (queues
    // EndUtterance). Idle -> ignored (stale end — e.g. after a cap force
    // finalize while the VAD was still speaking). Finalizing -> a pending
    // barge-in ended inside the finalize window (its audio went to the current
    // final): drop the pending reopen.
    void on_vad_end(int64_t sample);

    // Called once per fed chunk with the chunk's end sample. Enforces the
    // utterance cap: Speaking for >= cap_samples force-finalizes (queues
    // EndUtterance) so an infinite monologue never blocks the pipeline
    // (ISC-9).
    void tick(int64_t end_sample);

    // Forces the current utterance to finalize right now (daemon shutdown /
    // fixture EOF while the VAD never emitted a SpeechEnd). No-op unless
    // Speaking.
    void force_finalize(int64_t end_sample);

    // ---- drained by the daemon loop ----
    Intent next_intent();

    // Call after an EndUtterance intent has been resolved (finalize done):
    // completes the Finalizing -> Idle transition, or reopens the next
    // utterance immediately if a barge-in was pending during finalize (queue
    // depth 1). Returns true if a BeginUtterance was queued.
    bool reopen_pending();

    // Drops the current utterance and all queued intents (e.g. after
    // stt.begin_utterance() failed — the daemon emitted speech.error). The
    // next real SpeechStart opens a fresh utterance.
    void abort_utterance();

    // ---- state accessors ----
    State state() const { return state_; }
    // True while the machine consumes audio for an open utterance (feed the
    // STT session).
    bool audio_active() const {
        return state_ == State::Speaking || state_ == State::Finalizing;
    }
    // Sequence of the current (or most recently begun) utterance, 1-based.
    int seq() const { return seq_; }
    int64_t start_sample() const { return start_sample_; }
    int64_t end_sample() const { return end_sample_; }

private:
    void enqueue(Intent it) { intents_.push_back(it); }

    int64_t cap_samples_;
    State state_ = State::Idle;
    int64_t start_sample_ = 0;        // SpeechStart sample of current utterance
    int64_t end_sample_ = 0;          // SpeechEnd / cap sample (for duration_ms)
    int seq_ = 0;                     // per-utterance seq, incremented at begin
    bool pending_ = false;            // barge-in seen while Finalizing
    int64_t pending_start_sample_ = 0;
    std::deque<Intent> intents_;
};

}  // namespace persona
