#include "config.h"
#include "model/registry.h"
#include "pipeline/vad.h"

#include "engine/framework/runtime/registry.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace persona {

// Defined in src/listen.cpp (T3).
int verb_listen(const Config& cfg, const std::vector<std::string>& args);

// Defined in src/models.cpp (T4).
int verb_models(const Config& cfg, const std::vector<std::string>& args);

// Defined in src/devices.cpp (T6).
int verb_devices(const Config& cfg, const std::vector<std::string>& args);

// Defined in src/tts.cpp (T10).
int verb_tts(const Config& cfg, const std::vector<std::string>& args);

// Defined in src/daemon/daemon.cpp (T9).
int verb_daemon(const Config& cfg, const std::vector<std::string>& args);

namespace {

constexpr const char* kVersion = "persona 0.1.0";

void print_usage() {
    std::cout <<
        "persona — voice daemon on audio.cpp\n"
        "\n"
        "Usage:\n"
        "  persona <verb> [options]\n"
        "\n"
        "Verbs:\n"
        "  selftest     Load the silero_vad runtime and print the loader catalog\n"
        "               --vad: stream synthetic audio through silero_vad and assert endpointing\n"
        "  models       Model catalog: search, list, info, install, uninstall\n"
        "  devices      List audio capture/playback devices (PortAudio)\n"
        "  listen       Transcribe a WAV file or stdin (--stdin --streaming for streaming ASR)\n"
        "  tts          Synthesize speech: tts [--out <file.wav>] [--play] <text>\n"
        "               (text from args or stdin; default --out - = WAV to stdout)\n"
        "  daemon       Continuous mic -> NDJSON voice channel (endpointing)\n"
        "               --mic none --audio-fixture <wav>: scripted test mode\n"
        "               --mic <idx>|default: capture device (global --mic-device also works)\n"
        "               --utt-cap-s <s> / --vad-min-silence-ms <ms>: endpoint tuning\n"
        "               --agent none|pi: hand utterances to the pi RPC agent (default none)\n"
        "               --pi-args <args>: extra args for pi (JSON array or space-separated)\n"
        "               --no-speak: log agent replies instead of speaking them\n"
        "               --no-interrupt: never abort an in-flight agent turn (interrupt is\n"
        "                               ON by default — new input flushes playback and\n"
        "                               aborts the agent via pi's abort command)\n"
        "               --web [--web-port <n>] [--web-addr <ip>]: browser voice UI —\n"
        "                               serves an HTML page at http://<addr>:<port>/ and\n"
        "                               streams audio over ws:// (mic in @ 16 kHz, TTS out\n"
        "                               @ 24 kHz); --web-port 0 = ephemeral (actual port\n"
        "                               logged to stderr); requires --mic none (or no --mic)\n"
        "\n"
        "Prompt seeding (--agent pi):\n"
        "  pi --mode rpc ignores positional messages and @file message args — the only way\n"
        "  to seed the agent at startup is the --append-system-prompt CLI flag, passed via\n"
        "  --pi-args. The value is text or a file path (@file), and the flag can be\n"
        "  repeated:\n"
        "    persona daemon --agent pi --pi-args '[\"--append-system-prompt\", \"You are\n"
        "      persona, a voice assistant. Reply in 1-2 short sentences.\"]'\n"
        "    persona daemon --agent pi --pi-args '[\"--append-system-prompt\", \"@/etc/\n"
        "      persona-prompt.txt\"]'   (file contents are appended; verified against pi)\n"
        "  --pi-args also accepts space-separated form: --pi-args '--append-system-prompt \"You\n"
        "  are…\"' (quote as a single shell argument). Positional messages on the daemon\n"
        "  command line do NOT reach pi in rpc mode."
        "\n"
        "Global options:\n"
        "  --models-root <dir>   Model storage root (default $XDG_DATA_HOME/persona/models)\n"
        "  --specs-dir <dir>     Model catalog dir (default $PERSONA_SPECS_DIR or compile-time)\n"
        "  --backend <name>      Compute backend (default cpu)\n"
        "  --mic-device <index>  Capture device index (listen --mic / daemon)\n"
        "  --play-device <index> Playback device index (tts --play / daemon TTS;\n"
        "                         marks a real device — incompatible with daemon --web)\n"
        "  --utt-cap-s <s>       Max utterance seconds before force-finalize (default 30)\n"
        "  --vad-min-silence-ms <ms>  Silence needed to end an utterance (default 800)\n"
        "  --vad-threshold <p>    VAD speech probability threshold, 0..1 (default 0.5)\n"
        "  --vad-min-speech-ms <ms>   Min speech to start an utterance (default 250)\n"
        "  --asr-family <name>    ASR model family (default qwen3_asr)\n"
        "  --asr-package <id>     ASR package variant (default: spec default, e.g. qwen3_asr_1_7b_q8_0)\n"
        "  --asr-language <code>  ASR language hint (default: empty = auto-detect);\n"
        "                         qwen3/sense: display name or code; nemotron: BCP-47\n"
        "  --tts-family <name>    TTS model family (default pocket_tts)\n"
        "  --tts-package <id>     TTS package variant (default: spec default)\n"
        "  --version             Print version and exit\n"
        "  --help                Print this help and exit\n";
}

// selftest --vad: synthetic audio smoke test for the VadSession wrapper (T7).
// Synthesizes 2 s of 16 kHz mono f32 — 1 s silence, 0.5 s of a 440 Hz tone
// (amplitude 0.5), 0.5 s silence — and streams it through the streaming
// silero_vad session in preferred-sized chunks, asserting exactly one
// SpeechStart and one SpeechEnd with end > start.
int verb_selftest_vad(const Config& cfg) {
    Runtime rt = make_runtime(cfg);
    if (!rt.vad_model) {
        std::cerr << "selftest --vad: silero_vad runtime not loaded\n";
        return 1;
    }

    constexpr int kRate = 16000;
    std::vector<float> audio;
    audio.reserve(static_cast<size_t>(2 * kRate));
    audio.insert(audio.end(), kRate, 0.0f);  // 1 s silence

    // 0.5 s of a voiced, harmonic-rich synthetic "tone": a sawtooth-like stack
    // (150 Hz fundamental + 12 harmonics) amplitude-modulated at 4 Hz, then
    // normalized to 0.8 peak. EXPERIMENTAL FINDING: a pure 440 Hz sine does NOT
    // trigger silero_vad (its probability is ~0 — the model is trained on
    // speech spectra, not pure tones, and rejects uniform noise too). This
    // harmonic stack crosses the default threshold 0.5 reliably without any
    // option tuning.
    {
        const int n = kRate / 2;
        for (int i = 0; i < n; ++i) {
            const double t = static_cast<double>(i) / kRate;
            double v = 0.0;
            for (int h = 1; h <= 12; ++h) {
                v += (1.0 / h) * std::sin(2.0 * 3.14159265358979323846 * 150.0 * h * t);
            }
            const double env = 0.5 * (1.0 + std::sin(2.0 * 3.14159265358979323846 * 4.0 * t + 1.0));
            audio.push_back(static_cast<float>(0.8 * env * v));
        }
        double peak = 0.0;
        for (size_t i = static_cast<size_t>(kRate); i < audio.size(); ++i) {
            peak = std::max(peak, static_cast<double>(std::fabs(audio[i])));
        }
        for (size_t i = static_cast<size_t>(kRate); i < audio.size(); ++i) {
            audio[i] = static_cast<float>(audio[i] / peak * 0.8);
        }
    }
    audio.insert(audio.end(), kRate / 2, 0.0f);  // 0.5 s trailing silence

    // The callbacks are void() per the wrapper contract, so the reported sample
    // position is the chunk start being fed when the transition fired (within
    // one 512-sample chunk of the engine's exact event sample — silero backs
    // the start up by speech_pad_ms and pads the end forward).
    int starts = 0;
    int ends = 0;
    int64_t start_sample = -1;
    int64_t end_sample = -1;
    bool speaking = false;
    int64_t cur_chunk_start = 0;
    VadSession::Events ev;
    ev.on_speech_start = [&] {
        speaking = true;
        ++starts;
        start_sample = cur_chunk_start;
    };
    ev.on_speech_end = [&] {
        speaking = false;
        ++ends;
        end_sample = cur_chunk_start;
    };

    VadSession vad(*rt.vad_model, ev);
    vad.start();
    const int64_t chunk = vad.chunk_samples();
    int64_t pos = 0;
    for (size_t i = 0; i + static_cast<size_t>(chunk) <= audio.size(); i += static_cast<size_t>(chunk)) {
        cur_chunk_start = pos;
        const std::vector<float> slice(audio.begin() + static_cast<long>(i),
                                       audio.begin() + static_cast<long>(i + static_cast<size_t>(chunk)));
        vad.feed(slice, pos);
        pos += chunk;
    }
    vad.finish();

    std::cout << "vad_speech_start_sample=" << start_sample << "\n";
    std::cout << "vad_speech_end_sample=" << end_sample << "\n";
    std::cout << "vad_speech_starts=" << starts << " vad_speech_ends=" << ends
              << " vad_speaking_at_finish=" << (speaking ? "yes" : "no") << "\n";

    const bool ok = (starts == 1 && ends == 1 && end_sample > start_sample && !speaking);
    if (ok) {
        std::cout << "selftest --vad: OK\n";
        return 0;
    }
    std::cerr << "selftest --vad: FAILED (expected exactly one SpeechStart then one SpeechEnd, "
              << "end > start)\n";
    return 1;
}

int verb_selftest(const Config& cfg, const std::vector<std::string>& args) {
    for (const auto& arg : args) {
        if (arg == "--vad") {
            return verb_selftest_vad(cfg);
        }
    }
    Runtime rt = make_runtime(cfg);

    const auto loaders = rt.registry.advertise_loaders();
    std::cout << "registered_loaders=" << loaders.size() << "\n";
    for (const auto& loader : loaders) {
        std::cout << loader.family << ":";
        for (const auto& task : loader.capabilities.supported_tasks) {
            std::cout << " " << engine::runtime::to_string(task.task) << " (";
            for (size_t m = 0; m < task.modes.size(); ++m) {
                if (m > 0) {
                    std::cout << "|";
                }
                std::cout << engine::runtime::to_string(task.modes[m]);
            }
            std::cout << ")";
        }
        std::cout << "\n";
    }

    std::cout << "silero_vad_loaded=" << (rt.vad_model ? "yes" : "no") << "\n";
    if (!rt.vad_model) {
        std::cerr << "selftest: failed to load silero_vad runtime\n";
        return 1;
    }
    std::cout << "asr_loaded=" << (rt.asr_model ? "yes" : "no")
              << " (models_root=" << cfg.models_root << ")\n";
    std::cout << "tts_loaded=" << (rt.tts_model ? "yes" : "no") << "\n";

    // Require at least one loader advertising silero_vad (link+runtime proof).
    for (const auto& loader : loaders) {
        if (loader.family == "silero_vad") {
            std::cout << "selftest: OK\n";
            return 0;
        }
    }
    std::cerr << "selftest: silero_vad loader missing from registry\n";
    return 1;
}

// Dispatch table: verb -> handler. Stateless lambdas convert to function
// pointers, so later todos add one row per new verb without touching dispatch.
using VerbFn = int (*)(const Config&, const std::vector<std::string>&);

int verb_not_implemented(const Config&, const std::vector<std::string>&) {
    return 1;  // caller prints the message (dispatch knows the verb name)
}

int dispatch(const CliArgs& args) {
    if (args.verb.empty()) {
        print_usage();
        return 1;
    }
    static const struct {
        const char* name;
        VerbFn fn;
    } kVerbs[] = {
        {"selftest", verb_selftest},
        {"listen",   verb_listen},
        {"models",   verb_models},
        {"devices",  verb_devices},
        {"tts",      verb_tts},
        {"daemon",   verb_daemon},
    };
    for (const auto& verb : kVerbs) {
        if (args.verb == verb.name) {
            if (verb.fn == verb_not_implemented) {
                std::cerr << "persona: verb '" << args.verb << "' is not implemented yet\n";
                return 1;
            }
            return verb.fn(args.config, args.verb_args);
        }
    }
    std::cerr << "persona: unknown verb '" << args.verb << "'\n\n";
    print_usage();
    return 1;
}

}  // namespace
}  // namespace persona

int main(int argc, char** argv) {
    try {
        // --version / --help are handled before dispatch regardless of verb.
        for (int i = 1; i < argc; ++i) {
            const std::string arg(argv[i]);
            if (arg == "--version") {
                std::cout << persona::kVersion << "\n";
                return 0;
            }
            if (arg == "--help") {
                persona::print_usage();
                return 0;
            }
        }
        return persona::dispatch(persona::parse_args(argc, argv));
    } catch (const std::exception& ex) {
        std::cerr << "persona: " << ex.what() << "\n";
        return 1;
    }
}
