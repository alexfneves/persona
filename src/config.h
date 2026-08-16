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
    // Compute backend for engine sessions. Defaults to the backend this
    // binary was built for (PERSONA_DEFAULT_BACKEND: "cpu" for .#persona,
    // "vulkan" for .#persona-vulkan); --backend overrides it.
    std::string backend;
    // Microphone device index for capture (listen --mic / daemon). -1 = the
    // PortAudio default input device. Set with --mic-device <index>.
    int mic_device = -1;
    // Playback device index for tts --play / daemon TTS. -1 = the PortAudio
    // default output device. Set with --play-device <index> (T10; used by T11).
    int play_device = -1;
    // Utterance cap in seconds (daemon endpointing): speech longer than this is
    // force-finalized so an infinite monologue never blocks the pipeline
    // (ISC-9). Set with --utt-cap-s (default 30).
    int utt_cap_s = 30;
    // VAD min_silence_duration_ms (daemon): sustained silence at or above this
    // triggers the silero SpeechEnd that finalizes an utterance. 1 s is a
    // comfortable endpoint latency for natural speech (mid-phrase pauses
    // rarely exceed 1 s; inter-utterance gaps are typically longer). Set with
    // --vad-min-silence-ms (default 1000).
    int vad_min_silence_ms = 1000;
    // Agent mode (daemon): "none" = agent-agnostic NDJSON (default), "pi" =
    // spawn `pi --mode rpc` and hand each speech.final to it (Decision 8).
    // Set with --agent none|pi (T12).
    std::string agent = "none";
    // Model selection (T13): family + optional package id for ASR and TTS
    // (Decision 2 — family/package ids are config, not hardcoded). An empty
    // package id means "the spec's default package". Validated against the
    // catalog (fail fast with a hint on unknown family/package); the package
    // id is echoed in the daemon ready line (e.g. asr_package).
    std::string asr_family = "qwen3_asr";
    std::string asr_package;  // e.g. "qwen3_asr_0_6b_q8_0" ("" = spec default)
    // ASR language hint (--asr-language, default empty = auto-detect). Sent on
    // the TaskRequest as options["language"] AND text_input (qwen3_asr reads
    // ONLY text_input; both channels is the audio.cpp server pattern). Empty
    // keeps current behavior — family caveats: qwen3 wants English names
    // ("English"), nemotron BCP-47 ("en-US", validated), kroko throws on a
    // mismatch with its package language, higgs/voxtral/parakeet ignore it.
    std::string asr_language;
    std::string tts_family = "pocket_tts";
    std::string tts_package;  // e.g. "pocket_tts_english_q8_0" ("" = spec default)
    // VAD tuning (T13): passed into the silero SessionOptions.options as
    // {"threshold", "min_speech_duration_ms", "min_silence_duration_ms"}
    // (silero reads tuning ONLY from there, verified T7). --vad-threshold is a
    // speech probability in (0, 1] (default 0.5); --vad-min-speech-ms is the
    // minimum sustained speech before an utterance starts (default 250).
    double vad_threshold = 0.5;
    int vad_min_speech_ms = 250;
    // Extra args appended to `pi --mode rpc` (e.g. --provider, --model). Set
    // with --pi-args (JSON array or space-separated).
    std::vector<std::string> pi_args;
    // --no-speak: log agent replies instead of speaking them (agent.reply.done
    // with spoken:false, no TTS run).
    bool no_speak = false;
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
