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
    // Microphone device index for capture (listen --mic / daemon). -1 = the
    // PortAudio default input device. Set with --mic-device <index>.
    int mic_device = -1;
    // Utterance cap in seconds (daemon endpointing): speech longer than this is
    // force-finalized so an infinite monologue never blocks the pipeline
    // (ISC-9). Set with --utt-cap-s (default 30).
    int utt_cap_s = 30;
    // VAD min_silence_duration_ms (daemon): sustained silence at or above this
    // triggers the silero SpeechEnd that finalizes an utterance. 800 ms sits
    // above the 0.51 s intra-utterance pause of the hello.wav fixture (so it
    // stays ONE utterance) and below the 2 s inter-utterance gap of
    // hello_hello.wav (so endpointing still splits them — ISC-6). A T13
    // tuning knob; set with --vad-min-silence-ms.
    int vad_min_silence_ms = 800;
};

// Parsed command line. The verb is the first non-flag argument (flags and
// their values are consumed by parse_args); everything after it is handed to
// the verb handler verbatim so later verbs can parse their own flags.
struct CliArgs {
    Config config;
    std::string verb;
    std::vector<std::string> verb_args;
};

// Parses global flags (--models-root, --specs-dir, --backend, --mic-device)
// from anywhere on the command line and extracts the verb as the first
// remaining non-flag arg. Unknown flags are preserved in CliArgs::verb_args for
// verb-level parsing. Throws std::runtime_error on malformed global flags.
CliArgs parse_args(int argc, char** argv);

}  // namespace persona
