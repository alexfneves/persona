#include "audio/capture.h"
#include "audio/wav.h"
#include "model/registry.h"

#include "engine/framework/runtime/session.h"

#include <chrono>
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

void print_transcript(const engine::runtime::TaskResult& res) {
    if (res.text_output && !res.text_output->text.empty()) {
        std::cout << res.text_output->text << "\n";
    } else {
        std::cout << "(empty)\n";
    }
}

// One offline ASR run over a complete mono 16 kHz f32 buffer. All engine
// calls happen on this (single) thread.
int run_offline(const Runtime& rt, const std::vector<float>& samples) {
    if (!rt.asr_model) {
        std::cerr << "listen: ASR model not loaded\n"
                  << "  install it with:  persona models install qwen3_asr\n";
        return 1;
    }
    engine::runtime::SessionOptions opts;
    opts.backend.type = engine::core::BackendType::Cpu;
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
// persona listen --mic        — capture ~3 s from the mic, then transcribe
int verb_listen(const Config& cfg, const std::vector<std::string>& args) {
    Runtime rt = make_runtime(cfg);

    if (args.size() != 1) {
        std::cerr << "usage:\n"
                  << "  persona listen <file.wav>   transcribe a WAV file\n"
                  << "  persona listen --stdin      transcribe raw s16le 16 kHz mono PCM from stdin\n"
                  << "  persona listen --mic        capture ~3 s from the mic, then transcribe\n";
        return 1;
    }

    const std::string& input = args[0];
    if (input == "--stdin") {
        return run_offline(rt, read_stdin_s16le_f32());
    }
    if (input == "--mic") {
        std::cerr << "listen: capturing 3 s of audio from the mic"
                  << (cfg.mic_device >= 0 ? " (device " + std::to_string(cfg.mic_device) + ")" : "")
                  << "...\n";
        const std::vector<float> samples = capture_mic_f32(cfg, 3.0);
        std::cerr << "listen: captured " << samples.size() << " samples ("
                  << samples.size() / 16000.0 << " s), transcribing...\n";
        return run_offline(rt, samples);
    }
    return run_offline(rt, read_wav_f32(input));
}

}  // namespace persona
