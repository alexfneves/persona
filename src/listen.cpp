#include "backend.h"

#include "audio/capture.h"
#include "audio/wav.h"
#include "model/registry.h"
#include "pipeline/stt.h"

#include "engine/framework/runtime/session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace persona {

namespace {

// Reads raw s16le 16 kHz mono PCM from stdin (the format `arecord`/`ffmpeg`
// produce with -f s16le -ar 16000 -ac 1) and converts to float32.
std::vector<float> read_stdin_s16le_f32() {
    const std::vector<char> raw((std::istreambuf_iterator<char>(std::cin)),
                                std::istreambuf_iterator<char>());
    std::vector<float> f32;
    f32.reserve(raw.size() / 2);
    for (size_t i = 0; i + 1 < raw.size(); i += 2) {
        const int16_t s = static_cast<int16_t>(
            static_cast<uint16_t>(static_cast<uint8_t>(raw[i])) |
            (static_cast<uint16_t>(static_cast<uint8_t>(raw[i + 1])) << 8));
        f32.push_back(static_cast<float>(s) / 32768.0f);
    }
    return f32;
}

// Captures `seconds` of mono audio at 16 kHz from the configured mic (default
// input device, or cfg.mic_device when set) into a float buffer, draining the
// ring continuously so the callback never overflows.
std::vector<float> capture_mic_f32(const Config& cfg, double seconds) {
    constexpr int kRate = 16000;

    Capture cap;
    if (cfg.mic_device >= 0) {
        cap.open_mic(cfg.mic_device, kRate);
    } else {
        cap.open_default_mic(kRate);
    }
    cap.start();

    std::vector<float> samples;
    samples.reserve(static_cast<size_t>(kRate * seconds));
    std::vector<float> tmp(4096);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(
                                               static_cast<int>(seconds * 1000));
    while (std::chrono::steady_clock::now() < deadline) {
        size_t n;
        while ((n = cap.ring().pop_up_to(tmp.data(), tmp.size())) > 0) {
            samples.insert(samples.end(), tmp.begin(),
                           tmp.begin() + static_cast<long>(n));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    cap.stop();
    size_t n;
    while ((n = cap.ring().pop_up_to(tmp.data(), tmp.size())) > 0) {
        samples.insert(samples.end(), tmp.begin(),
                       tmp.begin() + static_cast<long>(n));
    }
    return samples;
}

// RMS gate used by the --streaming utterance splitter: a chunk is "silent"
// when its RMS falls below this (16-bit s16le silence is ~0.0; speech in the
// fixtures reaches 0.09+).
bool chunk_is_silent(const std::vector<float>& chunk, float rms_threshold) {
    if (chunk.empty()) {
        return true;
    }
    double sum = 0.0;
    for (const float s : chunk) {
        sum += static_cast<double>(s) * static_cast<double>(s);
    }
    return std::sqrt(sum / static_cast<double>(chunk.size())) < rms_threshold;
}

// Streaming transcription of raw s16le 16 kHz mono PCM from stdin, chunked
// into 512-sample frames and fed through a per-utterance SttSession.
//
// Utterance segmentation here is a STAND-IN for the T9 endpointer (VAD): a
// run of >= 0.8 s of silent chunks ends the current utterance (fires on_final),
// the next speech onset begins a fresh one. 0.8 s sits between the
// intra-utterance pause of the fixture (0.51 s, "Hello, world." / "This is a
// test.") and the inter-utterance gap of the two-utterance fixture (2 s).
// This pre-validates the per-utterance session lifecycle without the VAD.
//
// KNOWN STAND-IN ARTIFACT: because the boundary must sit above the fixture's
// 0.51 s intra-utterance pause, the finalize window of an ended utterance
// always contains ~0.8 s of trailing silence, which qwen3 occasionally
// hallucinates over (the two-utterance finals may end "...a test. Yes.").
// The single-fixture final (EOF-ended, 0.42 s trailing silence) is clean.
// T9's VAD-driven endpointing removes the artifact entirely.
//
// Partials are printed to stdout with a `partial: ` prefix (tests grep for
// it); the final text goes to stdout on its own line.
int run_streaming_stdin(const Runtime& rt, const Config& cfg) {
    if (!rt.asr_model) {
        std::cerr << "listen: ASR model not loaded (" << rt.asr_family
                  << " / " << rt.asr_package << ")\n"
                  << "  install it with:  " << install_hint(rt.asr_family, cfg.asr_package) << "\n";
        return 1;
    }

    // The streaming path creates per-utterance SttSessions too — honor the
    // parsed backend (review P1-1: ASR must run on the GPU on a Vulkan build).
    engine::core::BackendConfig backend;
    {
        std::string berr;
        if (!persona::parse_backend(cfg.backend, backend.type, berr)) {
            std::cerr << "listen: " << berr << "\n";
            return 1;
        }
    }
    backend.device = 0;
    backend.threads =
        std::max(1, static_cast<int>(std::thread::hardware_concurrency()));

    SttSession::Events ev;
    ev.on_partial = [](std::string partial) {
        std::cout << "partial: " << partial << "\n";
    };
    ev.on_final = [](std::string final_text) {
        std::cout << final_text << "\n";
    };
    ev.on_error = [](std::string err) {
        std::cerr << "listen: asr error: " << err << "\n";
    };
    SttSession stt(*rt.asr_model, ev);

    constexpr int kChunk = 512;  // same chunk the VAD gets (ISC/plan)
    constexpr float kRmsThreshold = 0.01f;
    // 0.8 s of silence ends the utterance (see note above).
    constexpr int kGapChunks = static_cast<int>(0.8 * 16000 / kChunk);

    const std::vector<float> f32 = read_stdin_s16le_f32();
    bool speaking = false;
    bool first = true;
    int silence_chunks = 0;
    int64_t pos = 0;
    for (size_t i = 0; i + static_cast<size_t>(kChunk) <= f32.size();
         i += static_cast<size_t>(kChunk)) {
        const std::vector<float> slice(
            f32.begin() + static_cast<long>(i),
            f32.begin() + static_cast<long>(i + static_cast<size_t>(kChunk)));
        const bool silent = chunk_is_silent(slice, kRmsThreshold);
        if (speaking) {
            // Feed everything (silence included): qwen3's 1 s windows are
            // sensitive to the exact audio timeline — truncating pauses shifts
            // window boundaries mid-word and garbles the transcript. Silence
            // only drives the boundary counter.
            stt.feed(slice, pos);
            if (silent) {
                if (++silence_chunks >= kGapChunks) {
                    stt.end_utterance();
                    speaking = false;
                    silence_chunks = 0;
                }
            } else {
                silence_chunks = 0;
            }
        } else if (first || !silent) {
            // First chunk of the stream (feed its leading silence so qwen3's
            // 1 s windows align with the source exactly as the CLI does —
            // dropping it shifts window boundaries and garbles the transcript),
            // or speech onset after a gap boundary.
            stt.begin_utterance(backend);
            stt.feed(slice, pos);
            speaking = true;
            first = false;
            silence_chunks = 0;
        }
        pos += kChunk;
    }
    if (speaking) {
        stt.end_utterance();
    }
    return 0;
}

void print_transcript(const engine::runtime::TaskResult& res) {
    if (res.text_output && !res.text_output->text.empty()) {
        std::cout << res.text_output->text << "\n";
    } else {
        std::cout << "(empty)\n";
    }
}

// One offline ASR run over a complete mono 16 kHz f32 buffer. All engine
// calls happen on this (single) thread.
int run_offline(const Runtime& rt, const std::vector<float>& samples,
                const std::string& backend, const Config& cfg) {
    if (!rt.asr_model) {
        std::cerr << "listen: ASR model not loaded (" << rt.asr_family
                  << " / " << rt.asr_package << ")\n"
                  << "  install it with:  " << install_hint(rt.asr_family, cfg.asr_package) << "\n";
        return 1;
    }
    engine::runtime::SessionOptions opts;
    {
        std::string berr;
        if (!persona::parse_backend(backend, opts.backend.type, berr)) {
            std::cerr << "listen: " << berr << "\n";
            return 1;
        }
    }
    opts.backend.device = 0;
    opts.backend.threads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));

    auto sess = rt.asr_model->create_task_session(
        {engine::runtime::VoiceTaskKind::Asr, engine::runtime::RunMode::Offline},
        opts);
    auto* off = dynamic_cast<engine::runtime::IOfflineVoiceTaskSession*>(sess.get());
    if (off == nullptr) {
        std::cerr << "listen: selected task session does not support offline execution\n";
        return 1;
    }

    engine::runtime::TaskRequest req;
    req.audio_input = engine::runtime::AudioBuffer{16000, 1, samples};
    sess->prepare(engine::runtime::build_preparation_request(req));
    const auto res = off->run(req);
    print_transcript(res);
    return 0;
}

}  // namespace

// persona listen <file.wav>   — offline ASR transcription of a WAV file
// persona listen --stdin      — same, reading raw s16le 16k mono PCM from stdin
// persona listen --stdin --streaming — streaming ASR over stdin chunks:
//                             partials (`partial: ` prefix) + final text
// persona listen --mic        — capture ~3 s from the mic, then transcribe
int verb_listen(const Config& cfg, const std::vector<std::string>& args) {
    Runtime rt = make_runtime(cfg);

    const bool streaming = std::find(args.begin(), args.end(), "--streaming") != args.end();
    std::string input;
    for (const auto& arg : args) {
        if (arg != "--streaming") {
            if (!input.empty()) {
                std::cerr << "usage: listen takes a single input (file, --stdin, or --mic)\n";
                return 1;
            }
            input = arg;
        }
    }
    if (input.empty()) {
        std::cerr << "usage:\n"
                  << "  persona listen <file.wav>           transcribe a WAV file\n"
                  << "  persona listen --stdin              transcribe raw s16le 16 kHz mono PCM from stdin\n"
                  << "  persona listen --stdin --streaming  streaming ASR: partials (`partial: ` prefix) + final\n"
                  << "  persona listen --mic                capture ~3 s from the mic, then transcribe\n";
        return 1;
    }
    if (streaming && input != "--stdin") {
        std::cerr << "listen: --streaming is only supported with --stdin\n";
        return 1;
    }

    if (input == "--stdin") {
        if (streaming) {
            return run_streaming_stdin(rt, cfg);
        }
        return run_offline(rt, read_stdin_s16le_f32(), cfg.backend, cfg);
    }
    if (input == "--mic") {
        std::cerr << "listen: capturing 3 s of audio from the mic"
                  << (cfg.mic_device >= 0 ? " (device " + std::to_string(cfg.mic_device) + ")" : "")
                  << "...\n";
        const std::vector<float> samples = capture_mic_f32(cfg, 3.0);
        std::cerr << "listen: captured " << samples.size() << " samples ("
                  << samples.size() / 16000.0 << " s), transcribing...\n";
        return run_offline(rt, samples, cfg.backend, cfg);
    }
    return run_offline(rt, read_wav_f32(input), cfg.backend, cfg);
}

}  // namespace persona
