#pragma once

// PortAudio input capture (T6). Opens a mono input stream at a requested
// sample rate (16 kHz default) and pushes float32 samples into an internal
// SPSC RingBuffer from the PortAudio callback. The callback never blocks and
// never does engine work (ISC-A-2) — it only pushes into the ring buffer,
// dropping samples on overflow.
//
// Ownership: the ring buffer is owned by the Capture object; the consumer
// (pipeline / listen thread) drains it with ring().pop_up_to().

#include "audio/ringbuf.h"

#include <portaudio.h>

#include <string>

namespace persona {

class Capture {
public:
    // Ensures PortAudio is initialized exactly once for the process (static
    // guard). Throws std::runtime_error on PA init failure.
    Capture();
    ~Capture();

    // Non-copyable (owns a PA stream + ring buffer).
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    // Opens the default input device (or a specific device index) at the given
    // mono sample rate. The PA stream stays closed until start(). Throws
    // std::runtime_error with a device-aware message on failure.
    void open_default_mic(int rate = 16000);
    void open_mic(int device_index, int rate = 16000);

    void start();  // throws on PA error; callback begins pushing into ring()
    void stop();   // stops the stream; buffered samples remain readable

    bool is_open() const { return stream_ != nullptr; }
    int sample_rate() const { return sample_rate_; }
    int device_index() const { return device_index_; }

    // Consumer access to the captured samples (drain with pop_up_to). The ring
    // holds 512 ms of audio at the configured rate.
    RingBuffer& ring() { return ring_; }

private:
    // Sample format negotiated with the device: prefer float32; fall back to
    // int16 (converted to float in the callback) when the device rejects float.
    enum class SampleFormat { Float32, Int16 };

    // The PortAudio callback (passed as the stream's userData). Lives for the
    // stream's lifetime; the callback reads the ring pointer and format, never
    // allocates, locks, or calls the engine (ISC-A-2).
    struct CallbackCtx {
        RingBuffer* ring = nullptr;
        SampleFormat format = SampleFormat::Float32;
    };

    // Static member so the callback can touch private types; registered with
    // Pa_OpenStream as a plain C callback (userData = &cb_ctx_).
    static int pa_input_callback(const void* input, void* output,
                                 unsigned long frames,
                                 const PaStreamCallbackTimeInfo* time_info,
                                 PaStreamCallbackFlags status_flags,
                                 void* user_data);

    void open_stream_impl(int device_index, int rate);

    // 8192 float samples = 512 ms @ 16 kHz — deep enough for the consumer's
    // drain cadence without ever blocking the callback.
    RingBuffer ring_{8192};
    CallbackCtx cb_ctx_;
    int sample_rate_ = 0;
    int device_index_ = -1;
    void* stream_ = nullptr;  // PaStream*
};

}  // namespace persona