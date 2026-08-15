#include "config.h"

#include <cstdlib>
#include <filesystem>
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
    out.config.backend = "cpu";
    out.config.mic_device = -1;

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