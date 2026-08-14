#include "config.h"
#include "model/registry.h"

#include "engine/framework/runtime/registry.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace persona {

// Defined in src/listen.cpp (T3).
int verb_listen(const Config& cfg, const std::vector<std::string>& args);

// Defined in src/models.cpp (T4).
int verb_models(const Config& cfg, const std::vector<std::string>& args);

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
        "  models       Model catalog: search, list, info (install/uninstall planned)\n"
        "  devices      Enumerate audio devices (planned)\n"
        "  listen       Transcribe a WAV file or stdin\n"
        "  tts          Synthesize speech (planned)\n"
        "  daemon       Continuous mic -> agent voice channel (planned)\n"
        "\n"
        "Global options:\n"
        "  --models-root <dir>   Model storage root (default $XDG_DATA_HOME/persona/models)\n"
        "  --specs-dir <dir>     Model catalog dir (default $PERSONA_SPECS_DIR or compile-time)\n"
        "  --backend <name>      Compute backend (default cpu)\n"
        "  --version             Print version and exit\n"
        "  --help                Print this help and exit\n";
}

int verb_selftest(const Config& cfg) {
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
        {"selftest", [](const Config& cfg, const std::vector<std::string>&) { return verb_selftest(cfg); }},
        {"listen",   verb_listen},
        {"models",   verb_models},
        {"tts",      verb_not_implemented},
        {"devices",  verb_not_implemented},
        {"daemon",   verb_not_implemented},
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
