#pragma once

#include "model/catalog.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace persona {

// Result of install_package.
struct InstallResult {
    bool ok = false;
    std::string error;                // top-level error message when !ok
    uintmax_t total_bytes = 0;        // total bytes on disk (sum of file sizes)
};

// The manifest written at <models_root>/<family>/.persona-manifest.json.
// File paths are relative to models_root (e.g. "PocketTTS-GGUF/english/
// pocket-tts-english-q8_0.gguf") so models list and uninstall can resolve
// them without re-reading the spec.
struct FamilyManifest {
    std::string family;
    std::string package;
    std::string repo;
    std::string revision;
    std::string installed_at;  // ISO 8601
    std::vector<std::pair<std::string, uintmax_t>> files;  // (path, bytes)
};

// Downloads every file of `pkg` into <models_root>/<target_directory> (with
// strip_prefix applied), then writes the family manifest. Idempotent: files
// already present with a matching size are skipped unless force. One retry per
// file; failed downloads leave the .part file in place for resume. Progress
// goes to stderr; stdout stays clean (the caller prints the one machine
// readable line).
InstallResult install_package(const Spec& spec, const Package& pkg,
                              const std::filesystem::path& models_root,
                              bool force);

// Removes the family's installed files and manifest. Refuses (returns false
// with a message) if the target directory would escape models_root.
bool uninstall_family(const Spec& spec, const std::filesystem::path& models_root,
                      std::string& error);

// The local path (relative to models_root) a package file lands at after
// strip_prefix is applied — the exact install target (T5's stripped_path
// semantics). E.g. Package{target_directory="Qwen3-ASR-0.6B-GGUF",
// strip_prefix="Qwen3-ASR-0.6B-GGUF", files=["Qwen3-ASR-0.6B-GGUF/
// qwen3-asr-0.6b-q8_0.gguf"]} -> "Qwen3-ASR-0.6B-GGUF/qwen3-asr-0.6b-q8_0.gguf".
// Used by install (T5) and by the T13 installed-check in registry.cpp (the
// daemon verifies the SELECTED package's files exist before loading, so
// e.g. --asr-package qwen3_asr_1_7b_f16 on a dir holding only the q8_0 gguf
// is reported as not-installed instead of silently loading the wrong model).
std::filesystem::path package_file_target(const Package& pkg,
                                          const std::string& remote_file);

// Reads the family manifest (nullopt when absent or unparseable).
std::optional<FamilyManifest> read_manifest(const std::filesystem::path& models_root,
                                            const std::string& family);

}  // namespace persona
