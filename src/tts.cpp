#include "audio/playback.h"
#include "audio/wav.h"
#include "model/registry.h"
#include "pipeline/tts.h"

#include "engine/framework/core/backend.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace persona {

namespace {

// Reads all of stdin as the TTS text (used when no text argument is given).
// Trailing whitespace/newlines are trimmed so `echo "hello" | persona tts`
// doesn't synthesize "hello\n".
std::string read_stdin_text() {
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    return s;
}

void print_usage() {
    std::cerr << "usage:\n"
              << "  persona tts [--out <file.wav>] [--play] <text>\n"
              << "  echo \"text\" | persona tts [--out <file.wav>] [--play]\n"
              << "\n"
              << "  --out <file.wav>   write 16-bit PCM WAV (default: - = stdout, composable)\n"
              << "  --play             play through the output device (--play-device <idx>)\n"
              << "  TTS family/package: global --tts-family / --tts-package\n";
}

// Waits until every enqueued buffer has been fully written to the device
// (queue drained) or `timeout_ms` elapses. Returns true when drained.
bool wait_for_drain(Playback& pb, int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pb.queue().drained()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

}  // namespace

// persona tts — offline TTS synthesis with pocket_tts. Text comes from the
// remaining non-flag args (joined with spaces) or from stdin when no text is
// given. Output defaults to a WAV on stdout (--out -), which composes with
// `| aplay`; --out <file.wav> writes a file; --play feeds the PlaybackQueue
// and waits for the device to finish playing.
//
// Exit codes: 0 success; 1 user error (no text, bad flags); 2 model/engine
// failure (model missing or synthesis failed).
int verb_tts(const Config& cfg, const std::vector<std::string>& args) {
    std::string out_path = "-";
    bool play = false;
    std::vector<std::string> text_words;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--out") {
            if (i + 1 >= args.size()) {
                print_usage();
                return 1;
            }
            out_path = args[++i];
        } else if (a == "--play") {
            play = true;
        } else {
            text_words.push_back(a);
        }
    }

    std::string text;
    if (!text_words.empty()) {
        for (size_t i = 0; i < text_words.size(); ++i) {
            if (i > 0) {
                text += " ";
            }
            text += text_words[i];
        }
    } else {
        text = read_stdin_text();
    }
    if (text.empty()) {
        print_usage();
        return 1;  // user error: no text
    }

    Runtime rt = make_runtime(cfg);
    if (!rt.tts_model) {
        std::cerr << "tts: TTS model not loaded (" << rt.tts_family
                  << " / " << rt.tts_package << ")\n"
                  << "  install it with:  " << install_hint(rt.tts_family, cfg.tts_package) << "\n";
        return 2;  // model failure
    }

    engine::core::BackendConfig backend;
    backend.type = engine::core::BackendType::Cpu;
    backend.device = 0;
    backend.threads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));

    const TtsSession::Result res = TtsSession::run(*rt.tts_model, text, backend);
    if (!res.ok) {
        std::cerr << "tts: synthesis failed: " << res.error << "\n";
        return 2;  // engine failure
    }

    // Progress goes to stderr so stdout stays binary-clean (composable).
    std::cerr << "tts: synthesized " << res.samples.size() << " samples at "
              << res.sample_rate << " Hz ("
              << (static_cast<double>(res.samples.size()) / res.sample_rate) << " s)\n";

    if (play) {
        Playback pb;
        pb.open(cfg.play_device, 0);  // device default rate; buffers are resampled to it
        AudioBufferPcm buf;
        buf.sample_rate = res.sample_rate;
        buf.samples = res.samples;
        pb.queue().enqueue(std::move(buf));
        pb.start();
        std::cerr << "tts: playing on '" << pb.device_name() << "' (device "
                  << pb.device_index() << ", " << pb.sample_rate() << " Hz)...\n";
        const bool drained = wait_for_drain(pb, 30000);
        if (!drained) {
            std::cerr << "tts: warning: playback did not finish within 30 s\n";
        } else {
            std::cerr << "tts: playback finished\n";
        }
        pb.stop();
        return 0;
    }

    if (out_path == "-") {
        write_wav_stdout(res.sample_rate, res.samples);
    } else {
        try {
            write_wav_file(out_path, res.sample_rate, res.samples);
        } catch (const std::exception& ex) {
            std::cerr << "tts: " << ex.what() << "\n";
            return 1;  // user error: unwritable output path
        }
    }
    return 0;
}

}  // namespace persona
