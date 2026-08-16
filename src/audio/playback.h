#pragma once

// PortAudio output (T10). Two pieces:
//   * PlaybackQueue — a lock-free SPSC queue of audio buffers. The pipeline
//     thread is the single producer (enqueue); the PortAudio output callback
//     is the single consumer (pop). The queue is engine-free and
//     rate-agnostic: buffers keep their native sample rate (TTS models
//     synthesize at their own rate, e.g. pocket_tts at 24 kHz).
//   * Playback — the PortAudio wiring: opens a mono float32 output stream at
//     a FIXED device rate (device default, or the rate given to open()) and
//     the callback linear-resamples every popped buffer to that rate on the
//     fly.
//
// Sample-rate strategy (documented decision): open the stream once at a fixed
// device rate and linear-resample each buffer to it in the callback. The
// alternative — opening the stream at the FIRST buffer's rate and reopening
// when a later buffer differs — cannot be done from the callback (the stream
// is owned by the PA thread) and would make the daemon's playback lifecycle
// stateful across buffers; fixed-rate + cheap linear resampling (the same
// pattern T3's WAV reader uses) keeps the callback stateless and non-blocking.
//
// Callback contract (ISC-A-2): never blocks (SPSC pop, no locks), never
// allocates (the active buffer is moved out of the queue, not copied), never
// calls the engine. The only shared state the callback touches is the
// PlaybackQueue; the active-buffer resampler state it owns exclusively.

#include <portaudio.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace persona {

// One chunk of audio to play: native sample rate + interleaved float samples
// (mono). An engine-free copy of the data the TTS result carried.
struct AudioBufferPcm {
    int sample_rate = 0;
    std::vector<float> samples;  // mono interleaved
};

// Lock-free single-producer / single-consumer queue of audio buffers. Same
// monotonic-slot pattern as RingBuffer (T6): the producer writes a slot and
// publishes it with a release store on tail_; the consumer reads slots only
// after an acquire load of tail_ (and vice versa for the free-space check via
// head_). The producer only ever writes slots at least `capacity` ahead of the
// consumer's read position, so the slot ownership never overlaps.
//
// enqueue() never blocks: on a full queue it DROPS the buffer and returns
// false (mirrors the capture ring's drop-on-overflow). For TTS utterances
// (one buffer per synthesis, capacity 16) this never triggers in practice.
// flush() (producer) sets a discard flag the PA callback checks FIRST: the
// active buffer is dropped and every queued buffer drained — the barge-in
// mechanism (F3 interrupt). Consumer-owned state, no locks.
class PlaybackQueue {
public:
    explicit PlaybackQueue(size_t capacity = 16)
        : capacity_(capacity), mask_(capacity - 1), slots_(capacity) {
        // power-of-two contract: (capacity_ & (capacity_ - 1)) == 0
    }

    // Producer (pipeline thread) only. Non-blocking; false when the queue is
    // full (buffer dropped).
    bool enqueue(AudioBufferPcm buf) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        if (tail - head >= capacity_) {
            return false;  // full — drop on overflow
        }
        slots_[tail & mask_] = std::move(buf);
        tail_.store(tail + 1, std::memory_order_release);
        total_enqueued_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Consumer (PA callback) only. Moves the next buffer out; false when
    // empty. Never blocks.
    bool pop(AudioBufferPcm& out) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);
        if (head >= tail) {
            return false;
        }
        out = std::move(slots_[head & mask_]);
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Called by the consumer once a popped buffer has been FULLY written to
    // the device (the audio is audible / the device consumed it). Drives
    // drained() — the "when is the queue done" signal tts.done (T11) needs.
    void note_buffer_consumed() {
        consumed_.fetch_add(1, std::memory_order_relaxed);
    }

    // Producer (pipeline thread) only: sets the discard flag so the next PA
    // callback invocation drops the active buffer + drains the queue. Safe to
    // call from any thread (the flag is atomic). Non-blocking.
    void flush() { discard_.store(true, std::memory_order_release); }

    // Consumer (PA callback) only: checks the discard flag set by flush();
    // returns true if the producer requested a flush (the caller should drop
    // active + drain queued buffers). Non-blocking, lock-free.
    bool check_flush() {
        return discard_.exchange(false, std::memory_order_acq_rel);
    }

    // Buffers waiting to be played (approximate).
    size_t queued() const {
        const size_t tail = tail_.load(std::memory_order_acquire);
        const size_t head = head_.load(std::memory_order_acquire);
        return tail - head;
    }

    uint64_t total_enqueued() const {
        return total_enqueued_.load(std::memory_order_relaxed);
    }

    uint64_t consumed() const {
        return consumed_.load(std::memory_order_relaxed);
    }

    // True once every enqueued buffer has been fully played. Approximate (the
    // counters are read without a lock) — fine for drain timing.
    bool drained() const {
        return consumed_.load(std::memory_order_relaxed) >=
               total_enqueued_.load(std::memory_order_relaxed);
    }

private:
    size_t capacity_;
    size_t mask_;
    std::vector<AudioBufferPcm> slots_;
    std::atomic<size_t> head_{0};  // written by consumer, read by producer
    std::atomic<size_t> tail_{0};  // written by producer, read by consumer
    std::atomic<uint64_t> total_enqueued_{0};
    std::atomic<uint64_t> consumed_{0};
    std::atomic<bool> discard_{false};
};

class Playback {
public:
    // Ensures PortAudio is initialized exactly once for the process (static
    // guard; Pa_Initialize is refcounted, so this coexists with capture's
    // guard). Throws std::runtime_error on PA init failure.
    Playback();
    ~Playback();

    // Non-copyable (owns a PA stream + queue + callback state).
    Playback(const Playback&) = delete;
    Playback& operator=(const Playback&) = delete;

    // Opens a mono float32 output stream. device_index -1 = the PortAudio
    // default output device; rate 0 = the device's default sample rate (the
    // rate every queued buffer is linear-resampled to). The stream stays
    // closed until start(). Throws std::runtime_error with a device-aware
    // message on failure.
    void open(int device_index = -1, int rate = 0);

    void start();  // throws on PA error; the callback begins draining queue()
    void stop();   // stops the stream (buffered audio is discarded)

    bool is_open() const { return stream_ != nullptr; }
    int device_index() const { return device_index_; }
    int sample_rate() const { return out_rate_; }
    const std::string& device_name() const { return device_name_; }

    PlaybackQueue& queue() { return queue_; }
    void flush() { queue_.flush(); }

private:
    // State the PA callback touches. The callback thread owns everything
    // except queue_ (SPSC): the active-buffer resampler state below is
    // exclusively the callback's, so no locking is needed. queue_/out_rate
    // are set in open() before start(), i.e. before any callback runs.
    struct CallbackCtx {
        PlaybackQueue* queue = nullptr;
        int out_rate = 0;
        AudioBufferPcm active;  // buffer currently being played
        double pos = 0.0;       // fractional input position within active
        double step = 1.0;      // input samples per output sample (in/out rate)
        bool has = false;
    };

    // Static member so the callback can touch private types; registered with
    // Pa_OpenStream as a plain C callback (userData = &cb_ctx_).
    static int pa_output_callback(const void* input, void* output,
                                  unsigned long frames,
                                  const PaStreamCallbackTimeInfo* time_info,
                                  PaStreamCallbackFlags status_flags,
                                  void* user_data);

    PlaybackQueue queue_{16};
    CallbackCtx cb_ctx_;
    int out_rate_ = 0;
    int device_index_ = -1;
    std::string device_name_;
    void* stream_ = nullptr;  // PaStream*
};

}  // namespace persona
