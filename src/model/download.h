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

// Reads the family manifest (nullopt when absent or unparseable).
std::optional<FamilyManifest> read_manifest(const std::filesystem::path& models_root,
                                            const std::string& family);

}  // namespace persona
