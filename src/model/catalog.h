#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace persona {

// One downloadable variant of a model family (model_specs/<family>.json).
struct Package {
    std::string id;               // e.g. "qwen3_asr_1_7b_q8_0"
    std::string precision;        // e.g. "q8_0", "f16", "native"
    std::string format;           // "gguf" | "safetensors"
    std::string target_directory; // dir under models_root, e.g. "Qwen3-ASR-1.7B-GGUF"
    std::vector<std::string> files;
    std::string strip_prefix;     // remote path prefix removed from target paths (T5)
    bool is_default = false;      // "default": true in the spec

    // Optional per-package download override. Safetensors packages commonly
    // point at different (sometimes gated) repos than package_defaults
    // (e.g. pocket_tts_english_safetensors -> kyutai/pocket-tts, gated). When
    // absent, the spec's package_defaults.download (Spec::repo/revision/gated)
    // applies (T5 resolves the merged view).
    struct Download {
        std::string repo;
        std::string revision;  // "main" when the spec omits it
        bool gated = false;
    };
    std::optional<Download> download;
};

// One model family from the shipped catalog (Decision 9: the 47 spec JSONs
// shipped by audiocpp-lib *are* the searchable catalog — no live HF search).
struct Spec {
    std::string family;       // e.g. "qwen3_asr"
    std::string display_name; // e.g. "Qwen3-ASR"
    std::string category;     // "asr" | "tts" | "vad" | "audio_tools" | ...
    std::string description;
    std::string status;       // "supported" | "community" | "wip" | ...
    std::vector<std::string> tasks;     // e.g. ["asr"], ["tts","clone"]
    std::vector<std::string> modes;     // e.g. ["offline","streaming"]
    std::vector<std::string> languages; // codes: "en", "en-US", "zh", ...
    std::vector<Package> packages;

    // HF download defaults (package_defaults.download). Absent in some specs
    // (fun_asr_nano, sense_asr, ...) which carry download info per-package.
    std::optional<std::string> repo;
    std::optional<std::string> revision;
    bool gated = false;
};

// Reads every *.json in specs_dir and parses it. Malformed files are skipped
// with a warning on stderr — a broken spec must never take down the catalog
// verbs. Pure data: no network, no audio.cpp private APIs.
std::vector<Spec> load_catalog(const std::filesystem::path& specs_dir);

// Linear lookup by family (catalog is ~47 entries; a map would be ceremony).
// The family string is a lookup key only — never used as a filesystem path.
// Returns nullptr when the family is unknown.
const Spec* find_spec(const std::vector<Spec>& specs, std::string_view family);

}  // namespace persona
