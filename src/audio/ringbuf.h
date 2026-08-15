#pragma once

// Lock-free single-producer / single-consumer ring buffer of float samples
// (T6). The PortAudio callback is the single producer (push); the pipeline /
// listen thread is the single consumer (pop_up_to). The contract is strict:
//  - exactly ONE thread may call push() at a time (and never from the same
//    thread as pop_up_to — no concurrent access at all beyond this pair);
//  - exactly ONE thread may call pop_up_to() at a time;
//  - the producer never blocks: push() drops the sample on a full buffer
//    (ISC-A-2 — no blocking audio work inside the PortAudio callback).
//
// The design is the classic monotonic-counter SPSC queue: head_ and tail_ are
// ever-increasing absolute counts; the buffer index is (count & mask) with
// capacity a power of two. The producer publishes samples with a release store
// on tail_; the consumer reads them after an acquire load of tail_ (and vice
// versa for the free-space check). No locks, no allocation on the push/pop
// paths — all storage is preallocated in the constructor.

#include <atomic>
#include <cstddef>
#include <vector>

namespace persona {

class RingBuffer {
public:
    // capacity_pow2: number of float samples the buffer can hold. Must be a
    // power of two (8192 = 512 ms of audio at 16 kHz).
    explicit RingBuffer(size_t capacity_pow2)
        : capacity_(capacity_pow2), mask_(capacity_pow2 - 1), buf_(capacity_pow2) {
        // power-of-two contract: mask arithmetic below assumes it
        // (capacity_ & (capacity_ - 1)) == 0
    }

    // Producer (PortAudio callback) only. Never blocks; drops the sample when
    // the buffer is full so the callback always returns promptly. Returns
    // false iff the sample was dropped.
    bool push(const float& v) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        if (tail - head >= capacity_) {
            return false;  // full — drop on overflow
        }
        buf_[tail & mask_] = v;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer (pipeline / listen thread) only. Copies up to n samples out in
    // FIFO order; returns the number copied (0 when empty). Never blocks.
    size_t pop_up_to(float* out, size_t n) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);
        const size_t avail = tail - head < n ? tail - head : n;
        for (size_t i = 0; i < avail; ++i) {
            out[i] = buf_[(head + i) & mask_];
        }
        head_.store(head + avail, std::memory_order_release);
        return avail;
    }

    // Number of samples currently buffered. Safe to call from either thread
    // (approximate under concurrency).
    size_t size() const {
        const size_t tail = tail_.load(std::memory_order_acquire);
        const size_t head = head_.load(std::memory_order_acquire);
        return tail - head;
    }

    size_t capacity() const { return capacity_; }

private:
    size_t capacity_;
    size_t mask_;
    std::vector<float> buf_;  // preallocated in ctor; push/pop never allocate
    std::atomic<size_t> head_{0};  // written by consumer, read by producer
    std::atomic<size_t> tail_{0};  // written by producer, read by consumer
};

}  // namespace persona
