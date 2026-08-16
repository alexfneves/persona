#include "audio/capture.h"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace persona {

namespace {

[[noreturn]] void fail_pa(const char* where, PaError err) {
    throw std::runtime_error(std::string(where) + ": " + Pa_GetErrorText(err));
}

// One-time process-wide PortAudio init. A function-local static guarantees
// Pa_Initialize() runs exactly once (at first Capture construction) and
// Pa_Terminate() runs at exit. Streams are closed in ~Capture, which runs
// before this static's destructor (function-local statics are destroyed in
// reverse construction order, and every Capture is constructed after this).
struct PaGlobal {
    PaGlobal() {
        const PaError err = Pa_Initialize();
        if (err != paNoError) {
            fail_pa("portaudio: Pa_Initialize failed", err);
        }
    }
    // Pa_Terminate is deliberately NOT called here: on a wedged audio
    // backend it can block for seconds inside the static destructor, hanging
    // the process in exit() (observed: Ctrl+C left the daemon alive until
    // the next stdin event). The OS reclaims all PortAudio state when the
    // process exits — teardown is unnecessary for a CLI/daemon.
};

void ensure_pa() {
    static const PaGlobal g;  // init guard: constructed once, first call wins
}

}  // namespace

Capture::Capture() {
    ensure_pa();
}

Capture::~Capture() {
    if (stream_ != nullptr) {
        Pa_StopStream(static_cast<PaStream*>(stream_));  // best-effort
        Pa_CloseStream(static_cast<PaStream*>(stream_));
    }
}

// PortAudio's internal thread: pushes float samples into the ring buffer and
// returns immediately. Drop-on-overflow, never blocks, never calls the engine
// (ISC-A-2).
int Capture::pa_input_callback(const void* input, void*, unsigned long frames,
                               const PaStreamCallbackTimeInfo*,
                               PaStreamCallbackFlags, void* user_data) {
    auto* ctx = static_cast<CallbackCtx*>(user_data);
    RingBuffer& ring = *ctx->ring;
    if (ctx->format == SampleFormat::Float32) {
        const float* s = static_cast<const float*>(input);
        for (unsigned long i = 0; i < frames; ++i) {
            if (!ring.push(s[i])) {
                break;  // full — drop the remainder of this callback
            }
        }
    } else {
        const int16_t* s = static_cast<const int16_t*>(input);
        constexpr float kInt16Scale = 1.0f / 32768.0f;
        for (unsigned long i = 0; i < frames; ++i) {
            if (!ring.push(static_cast<float>(s[i]) * kInt16Scale)) {
                break;
            }
        }
    }
    return paContinue;
}

void Capture::open_default_mic(int rate) {
    const PaDeviceIndex def = Pa_GetDefaultInputDevice();
    if (def == paNoDevice) {
        throw std::runtime_error(
            "portaudio: no default input device found\n"
            "  run `persona devices` to list devices and pick one with --mic-device <index>");
    }
    open_mic(static_cast<int>(def), rate);
}

void Capture::open_mic(int device_index, int rate) {
    if (stream_ != nullptr) {
        throw std::runtime_error("portaudio: capture already open");
    }
    if (device_index < 0 ||
        device_index >= static_cast<int>(Pa_GetDeviceCount())) {
        throw std::runtime_error(
            "portaudio: invalid device index " + std::to_string(device_index) +
            " (device count is " + std::to_string(Pa_GetDeviceCount()) + ")\n"
            "  run `persona devices` to list valid indices");
    }
    open_stream_impl(device_index, rate);
}

void Capture::start() {
    if (stream_ == nullptr) {
        throw std::runtime_error("portaudio: cannot start a closed capture");
    }
    const PaError err = Pa_StartStream(static_cast<PaStream*>(stream_));
    if (err != paNoError) {
        fail_pa("portaudio: Pa_StartStream failed", err);
    }
}

void Capture::stop() {
    if (stream_ == nullptr) {
        return;
    }
    const PaError err = Pa_AbortStream(static_cast<PaStream*>(stream_));
    if (err != paNoError && err != paStreamIsStopped) {
        fail_pa("portaudio: Pa_AbortStream failed", err);
    }
}

bool Capture::stream_active() const {
    if (stream_ == nullptr) {
        return false;  // never started / already stopped: nothing can flow
    }
    const PaError st = Pa_IsStreamActive(static_cast<PaStream*>(stream_));
    if (st < 0) {
        // Negative = a PaError (stream state indeterminate) — treat as
        // inactive so the idle watchdog surfaces the condition.
        return false;
    }
    // Returns 1 while running, 0 when stopped (incl. a callback that
    // returned non-paContinue — e.g. the device erroring out mid-stream).
    return st == 1;
}

std::string Capture::stream_host_error() const {
    const PaHostErrorInfo* info = Pa_GetLastHostErrorInfo();
    if (info == nullptr || info->errorText == nullptr) {
        return {};
    }
    std::string s(info->errorText);
    // The host may append a trailing newline; trim it for single-line logs.
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

void Capture::open_stream_impl(int device_index, int rate) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(device_index);
    if (info == nullptr) {
        throw std::runtime_error("portaudio: Pa_GetDeviceInfo failed for device " +
                                 std::to_string(device_index));
    }
    if (info->maxInputChannels < 1) {
        throw std::runtime_error("portaudio: device '" + std::string(info->name) +
                                 "' has no input channels");
    }

    PaStreamParameters in;
    in.device = device_index;
    in.channelCount = 1;
    in.suggestedLatency = info->defaultLowInputLatency;
    in.hostApiSpecificStreamInfo = nullptr;

    // Prefer float32; if the device rejects it, fall back to int16 (converted
    // in the callback). The userData (cb_ctx_) is registered with the stream so
    // the callback knows which format to decode; it lives for the stream's
    // lifetime. framesPerBuffer = paFramesPerBufferUnspecified lets the host
    // choose the callback size.
    SampleFormat format = SampleFormat::Float32;
    in.sampleFormat = paFloat32;
    PaError err = Pa_OpenStream(&stream_, &in, nullptr, static_cast<double>(rate),
                                paFramesPerBufferUnspecified, paNoFlag,
                                &Capture::pa_input_callback, &cb_ctx_);
    if (err != paNoError) {
        in.sampleFormat = paInt16;
        err = Pa_OpenStream(&stream_, &in, nullptr, static_cast<double>(rate),
                            paFramesPerBufferUnspecified, paNoFlag,
                            &Capture::pa_input_callback, &cb_ctx_);
        if (err != paNoError) {
            stream_ = nullptr;
            throw std::runtime_error(
                "portaudio: cannot open input on '" + std::string(info->name) +
                "' at " + std::to_string(rate) + " Hz (float32 or int16): " +
                Pa_GetErrorText(err));
        }
        format = SampleFormat::Int16;
    }

    cb_ctx_.ring = &ring_;
    cb_ctx_.format = format;
    sample_rate_ = rate;
    device_index_ = device_index;
}

}  // namespace persona
