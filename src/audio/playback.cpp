#include "audio/playback.h"

#include <stdexcept>
#include <string>

namespace persona {

namespace {

[[noreturn]] void fail_pa(const char* where, PaError err) {
    throw std::runtime_error(std::string(where) + ": " + Pa_GetErrorText(err));
}

// One-time process-wide PortAudio init (same pattern as capture.cpp). The two
// static guards coexist because Pa_Initialize/Pa_Terminate are refcounted —
// each call to Pa_Initialize must be paired with a matching Pa_Terminate, and
// the teardown only runs on the last one.
struct PaGlobal {
    PaGlobal() {
        const PaError err = Pa_Initialize();
        if (err != paNoError) {
            fail_pa("portaudio: Pa_Initialize failed", err);
        }
    }
    // Pa_Terminate deliberately skipped (see capture.cpp PaGlobal): on a
    // wedged backend it blocks in the static destructor, hanging exit().
};

void ensure_pa() {
    static const PaGlobal g;  // init guard: constructed once, first call wins
}

}  // namespace

Playback::Playback() {
    ensure_pa();
}

Playback::~Playback() {
    if (stream_ != nullptr) {
        Pa_StopStream(static_cast<PaStream*>(stream_));  // best-effort
        Pa_CloseStream(static_cast<PaStream*>(stream_));
    }
}

// PortAudio's output thread: pops queued buffers and writes their samples to
// the device, linear-resampling each buffer from its native rate to the fixed
// stream rate. Never blocks (SPSC pop, no locks), never allocates (the active
// buffer is moved out of the queue, not copied), never calls the engine
// (ISC-A-2). Silences the tail when the queue runs dry so the device never
// sees uninitialized samples.
int Playback::pa_output_callback(const void*, void* output, unsigned long frames,
                                 const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags,
                                 void* user_data) {
    auto* ctx = static_cast<CallbackCtx*>(user_data);
    float* out = static_cast<float*>(output);
    size_t produced = 0;
    while (produced < frames) {
        if (!ctx->has) {
            if (!ctx->queue->pop(ctx->active)) {
                break;  // nothing queued — silence the remainder below
            }
            ctx->pos = 0.0;
            ctx->step = ctx->active.sample_rate > 0
                            ? static_cast<double>(ctx->active.sample_rate) / ctx->out_rate
                            : 1.0;
            ctx->has = true;
        }
        const size_t n = ctx->active.samples.size();
        while (produced < frames && ctx->pos < static_cast<double>(n)) {
            const size_t i0 = static_cast<size_t>(ctx->pos);
            const size_t i1 = i0 + 1 < n ? i0 + 1 : i0;
            const double frac = ctx->pos - static_cast<double>(i0);
            out[produced++] = static_cast<float>(
                ctx->active.samples[i0] * (1.0 - frac) + ctx->active.samples[i1] * frac);
            ctx->pos += ctx->step;
        }
        if (ctx->pos >= static_cast<double>(n)) {
            // The whole buffer has been written to the device (audio heard).
            ctx->has = false;
            ctx->queue->note_buffer_consumed();
        }
    }
    for (; produced < frames; ++produced) {
        out[produced] = 0.0f;
    }
    return paContinue;
}

void Playback::open(int device_index, int rate) {
    if (stream_ != nullptr) {
        throw std::runtime_error("portaudio: playback already open");
    }
    int dev = device_index;
    if (dev < 0) {
        dev = Pa_GetDefaultOutputDevice();
        if (dev == paNoDevice) {
            throw std::runtime_error(
                "portaudio: no default output device found\n"
                "  run `persona devices` to list devices and pick one with --play-device <index>");
        }
    }
    if (dev < 0 || dev >= static_cast<int>(Pa_GetDeviceCount())) {
        throw std::runtime_error(
            "portaudio: invalid device index " + std::to_string(device_index) +
            " (device count is " + std::to_string(Pa_GetDeviceCount()) + ")\n"
            "  run `persona devices` to list valid indices");
    }
    const PaDeviceInfo* info = Pa_GetDeviceInfo(dev);
    if (info == nullptr) {
        throw std::runtime_error("portaudio: Pa_GetDeviceInfo failed for device " +
                                 std::to_string(dev));
    }
    if (info->maxOutputChannels < 1) {
        throw std::runtime_error("portaudio: device '" + std::string(info->name) +
                                 "' has no output channels");
    }
    const int out_rate = rate > 0 ? rate : static_cast<int>(info->defaultSampleRate);

    PaStreamParameters out;
    out.device = dev;
    out.channelCount = 1;
    out.sampleFormat = paFloat32;
    out.suggestedLatency = info->defaultLowOutputLatency;
    out.hostApiSpecificStreamInfo = nullptr;

    const PaError err = Pa_OpenStream(&stream_, nullptr, &out,
                                      static_cast<double>(out_rate),
                                      paFramesPerBufferUnspecified, paNoFlag,
                                      &Playback::pa_output_callback, &cb_ctx_);
    if (err != paNoError) {
        stream_ = nullptr;
        throw std::runtime_error(
            "portaudio: cannot open output on '" + std::string(info->name) +
            "' at " + std::to_string(out_rate) + " Hz (float32): " + Pa_GetErrorText(err));
    }

    cb_ctx_.queue = &queue_;
    cb_ctx_.out_rate = out_rate;
    cb_ctx_.has = false;
    out_rate_ = out_rate;
    device_index_ = dev;
    device_name_ = info->name;
}

void Playback::start() {
    if (stream_ == nullptr) {
        throw std::runtime_error("portaudio: cannot start a closed playback");
    }
    const PaError err = Pa_StartStream(static_cast<PaStream*>(stream_));
    if (err != paNoError) {
        fail_pa("portaudio: Pa_StartStream failed", err);
    }
}

void Playback::stop() {
    if (stream_ == nullptr) {
        return;
    }
    const PaError err = Pa_AbortStream(static_cast<PaStream*>(stream_));
    if (err != paNoError && err != paStreamIsStopped) {
        fail_pa("portaudio: Pa_AbortStream failed", err);
    }
}

}  // namespace persona
