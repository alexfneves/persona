#pragma once

#include <string>
#include <vector>

namespace persona {

// Runtime configuration resolved from defaults, environment, and CLI flags.
struct Config {
    // Where downloaded models live. Default: $XDG_DATA_HOME/persona/models,
    // else ~/.local/share/persona/models; overridable with PERSONA_MODELS_ROOT
    // or --models-root.
    std::string models_root;
    // Directory holding the model catalog (model_specs/*.json). Default:
    // compile-time PERSONA_SPECS_DIR if defined, else $PERSONA_SPECS_DIR env,
    // else the relative ../share/persona/model_specs.
    std::string specs_dir;
    // Compute backend for engine sessions. Only "cpu" is wired up for now.
    std::string backend = "cpu";
};

// Parsed command line. The verb is the first non-flag argument (flags and
// their values are consumed by parse_args); everything after it is handed to
// the verb handler verbatim so later verbs can parse their own flags.
struct CliArgs {
    Config config;
    std::string verb;
    std::vector<std::string> verb_args;
};

// Parses global flags (--models-root, --specs-dir, --backend) from anywhere on
// the command line and extracts the verb as the first remaining non-flag arg.
// Unknown flags are preserved in CliArgs::verb_args for verb-level parsing.
// Throws std::runtime_error on malformed global flags.
CliArgs parse_args(int argc, char** argv);

}  // namespace persona
