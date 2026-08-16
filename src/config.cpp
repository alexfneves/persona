#include "backend.h"
#include "config.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
namespace persona {

namespace fs = std::filesystem;

namespace {

std::string getenv_str(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

// $XDG_DATA_HOME/persona/models, else ~/.local/share/persona/models. Honors
// $HOME for ~ expansion (empty HOME falls back to the literal XDG default).
std::string default_models_root() {
    const std::string data_home = getenv_str("XDG_DATA_HOME");
    if (!data_home.empty()) {
        return (fs::path(data_home) / "persona" / "models").string();
    }
    const std::string home = getenv_str("HOME");
    if (!home.empty()) {
        return (fs::path(home) / ".local" / "share" / "persona" / "models").string();
    }
    return (fs::path(std::string(".")) / ".local" / "share" / "persona" / "models").string();
}

std::string default_specs_dir() {
#ifdef PERSONA_SPECS_DIR
    return std::string(PERSONA_SPECS_DIR);
#else
    // Runtime: PERSONA_SPECS_DIR env, else relative to the binary. (argv[0]
    // resolution is layered on top in parse_args.)
    const std::string env = getenv_str("PERSONA_SPECS_DIR");
    if (!env.empty()) {
        return env;
    }
    return "../share/persona/model_specs";
#endif
}

}  // namespace

CliArgs parse_args(int argc, char** argv) {
    CliArgs out;
    out.config.models_root = default_models_root();
    out.config.specs_dir = default_specs_dir();
    out.config.backend = persona::default_backend();
    out.config.mic_device = -1;
    out.config.play_device = -1;

    bool verb_found = false;
    std::vector<std::string> rest;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--models-root") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--models-root requires a path argument");
            }
            out.config.models_root = argv[++i];
            continue;
        }
        if (arg == "--specs-dir") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--specs-dir requires a path argument");
            }
            out.config.specs_dir = argv[++i];
            continue;
        }
        if (arg == "--backend") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--backend requires a value argument");
            }
            out.config.backend = argv[++i];
            continue;
        }
        if (arg == "--mic-device") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--mic-device requires a device index argument");
            }
            try {
                out.config.mic_device = std::stoi(argv[++i]);
            } catch (const std::exception&) {
                throw std::runtime_error("--mic-device expects a numeric device index, got '" +
                                         std::string(argv[i]) + "'");
            }
            continue;
        }
        if (arg == "--play-device") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--play-device requires a device index argument");
            }
            try {
                out.config.play_device = std::stoi(argv[++i]);
                out.config.have_play_device = true;
            } catch (const std::exception&) {
                throw std::runtime_error("--play-device expects a numeric device index, got '" +
                                         std::string(argv[i]) + "'");
            }
            continue;
        }
        if (arg == "--utt-cap-s") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--utt-cap-s requires a seconds argument");
            }
            try {
                out.config.utt_cap_s = std::stoi(argv[++i]);
            } catch (const std::exception&) {
                throw std::runtime_error("--utt-cap-s expects a numeric seconds value, got '" +
                                         std::string(argv[i]) + "'");
            }
            if (out.config.utt_cap_s <= 0) {
                throw std::runtime_error("--utt-cap-s must be positive");
            }
            continue;
        }
        if (arg == "--vad-min-silence-ms") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--vad-min-silence-ms requires a milliseconds argument");
            }
            try {
                out.config.vad_min_silence_ms = std::stoi(argv[++i]);
            } catch (const std::exception&) {
                throw std::runtime_error("--vad-min-silence-ms expects a numeric value, got '" +
                                         std::string(argv[i]) + "'");
            }
            if (out.config.vad_min_silence_ms <= 0) {
                throw std::runtime_error("--vad-min-silence-ms must be positive");
            }
            continue;
        }
        if (arg == "--vad-threshold") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--vad-threshold requires a value argument");
            }
            try {
                out.config.vad_threshold = std::stod(argv[++i]);
            } catch (const std::exception&) {
                throw std::runtime_error("--vad-threshold expects a numeric probability in (0, 1], got '" +
                                         std::string(argv[i]) + "'");
            }
            if (!(out.config.vad_threshold > 0.0 && out.config.vad_threshold <= 1.0)) {
                throw std::runtime_error("--vad-threshold must be in (0, 1] (a speech probability), got '" +
                                         std::string(argv[i]) + "'");
            }
            continue;
        }
        if (arg == "--vad-min-speech-ms") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--vad-min-speech-ms requires a milliseconds argument");
            }
            try {
                out.config.vad_min_speech_ms = std::stoi(argv[++i]);
            } catch (const std::exception&) {
                throw std::runtime_error("--vad-min-speech-ms expects a numeric value, got '" +
                                         std::string(argv[i]) + "'");
            }
            if (out.config.vad_min_speech_ms <= 0) {
                throw std::runtime_error("--vad-min-speech-ms must be positive");
            }
            continue;
        }
        if (arg == "--asr-family" || arg == "--asr-package" ||
            arg == "--asr-language" ||
            arg == "--tts-family" || arg == "--tts-package") {
            if (i + 1 >= argc) {
                throw std::runtime_error(arg + " requires a value argument");
            }
            const std::string val = argv[++i];
            if (val.empty()) {
                throw std::runtime_error(arg + " requires a non-empty value");
            }
            if (arg == "--asr-family") {
                out.config.asr_family = val;
            } else if (arg == "--asr-package") {
                out.config.asr_package = val;
            } else if (arg == "--asr-language") {
                out.config.asr_language = val;
            } else if (arg == "--tts-family") {
                out.config.tts_family = val;
            } else {
                out.config.tts_package = val;
            }
            continue;
        }
        if (arg == "--agent") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--agent requires none or pi");
            }
            const std::string val = argv[++i];
            if (val != "none" && val != "pi") {
                throw std::runtime_error("--agent expects none or pi, got '" + val + "'");
            }
            out.config.agent = val;
            continue;
        }
        if (arg == "--pi-args") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--pi-args requires a value");
            }
            const std::string val = argv[++i];
            if (!val.empty() && val.front() == '[') {
                // JSON array form: --pi-args '["--provider","openai"]'
                try {
                    const nlohmann::json arr = nlohmann::json::parse(val);
                    if (!arr.is_array()) {
                        throw std::runtime_error("not an array");
                    }
                    for (const auto& e : arr) {
                        if (!e.is_string()) {
                            throw std::runtime_error("non-string element");
                        }
                        out.config.pi_args.push_back(e.get<std::string>());
                    }
                } catch (const std::exception& ex) {
                    throw std::runtime_error(
                        "--pi-args expects a JSON array of strings, got '" + val + "': " +
                        ex.what());
                }
            } else {
                // Space-separated form: --pi-args '--provider openai'
                std::istringstream iss(val);
                std::string tok;
                while (iss >> tok) {
                    out.config.pi_args.push_back(tok);
                }
            }
            continue;
        }
        if (arg == "--no-speak") {
            out.config.no_speak = true;
            continue;
        }
        if (arg == "--no-interrupt") {
            out.config.interrupt = false;
            continue;
        }
        // First non-flag argument is the verb; everything henceforth (flags
        // included, globals are already consumed above) belongs to the verb.
        if (!verb_found && arg.size() > 0 && arg[0] != '-') {
            verb_found = true;
            out.verb = arg;
            continue;
        }
        rest.push_back(arg);
    }

    // Missing verb is not a parse error: dispatch() turns it into usage + exit 1.
    out.verb_args = std::move(rest);
    return out;
}

}  // namespace persona