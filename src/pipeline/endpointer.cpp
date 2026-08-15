#include "pipeline/endpointer.h"

namespace persona {

void Endpointer::on_vad_start(int64_t sample) {
    switch (state_) {
    case State::Idle:
        state_ = State::Speaking;
        start_sample_ = sample;
        ++seq_;
        enqueue(Intent::BeginUtterance);
        break;
    case State::Speaking:
        // Double SpeechStart while Speaking: ignore. VadSession dedupes by its
        // internal speaking_ flag, so this is purely defensive.
        break;
    case State::Finalizing:
        // Barge-in while finalizing: buffer only (sequential, queue depth 1).
        // The new utterance reopens the moment the current finalize completes.
        pending_ = true;
        pending_start_sample_ = sample;
        break;
    }
}

void Endpointer::on_vad_end(int64_t sample) {
    switch (state_) {
    case State::Idle:
        // Stale/missed SpeechEnd (e.g. after a cap force-finalize while the
        // VAD was still speaking): nothing to close — the next SpeechStart
        // opens a fresh utterance.
        break;
    case State::Speaking:
        state_ = State::Finalizing;
        end_sample_ = sample;
        enqueue(Intent::EndUtterance);
        break;
    case State::Finalizing:
        if (pending_) {
            // The barged-in speech ended inside the finalize window; its audio
            // was absorbed into the current final. Drop the pending reopen.
            pending_ = false;
        }
        break;
    }
}

void Endpointer::tick(int64_t end_sample) {
    if (state_ != State::Speaking) {
        return;
    }
    if (end_sample - start_sample_ >= cap_samples_) {
        // Utterance cap hit: force-finalize. end_sample_ is "now", not the
        // (absent) SpeechEnd sample, so duration_ms reflects the cap.
        state_ = State::Finalizing;
        end_sample_ = end_sample;
        enqueue(Intent::EndUtterance);
    }
}

void Endpointer::force_finalize(int64_t end_sample) {
    if (state_ != State::Speaking) {
        return;
    }
    state_ = State::Finalizing;
    end_sample_ = end_sample;
    enqueue(Intent::EndUtterance);
}

Endpointer::Intent Endpointer::next_intent() {
    if (intents_.empty()) {
        return Intent::None;
    }
    const Intent it = intents_.front();
    intents_.pop_front();
    return it;
}

bool Endpointer::reopen_pending() {
    if (!pending_) {
        // Finalize complete with no barge-in: back to Idle so the next
        // SpeechStart opens a fresh utterance (this is the ONLY place the
        // Finalizing -> Idle transition happens).
        state_ = State::Idle;
        return false;
    }
    pending_ = false;
    state_ = State::Speaking;
    start_sample_ = pending_start_sample_;
    ++seq_;
    enqueue(Intent::BeginUtterance);
    return true;
}

void Endpointer::abort_utterance() {
    state_ = State::Idle;
    pending_ = false;
    intents_.clear();
}

}  // namespace persona
