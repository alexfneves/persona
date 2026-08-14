#include "model/catalog.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>

namespace persona {

namespace fs = std::filesystem;

namespace {

// Best-effort string field: absent / null / non-string → fallback.
std::string json_str(const nlohmann::json& obj, const char* key,
                     const std::string& fallback = "") {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) {
        return fallback;
    }
    return it->get<std::string>();
}

// Best-effort string array: absent / non-array → empty; non-string entries
// are skipped.
std::vector<std::string> json_str_array(const nlohmann::json& obj, const char* key) {
    std::vector<std::string> out;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array()) {
        return out;
    }
    for (const auto& v : *it) {
        if (v.is_string()) {
            out.push_back(v.get<std::string>());
        }
    }
    return out;
}

Spec parse_spec(const nlohmann::json& j) {
    Spec spec;
    spec.family = json_str(j, "family");
    spec.display_name = json_str(j, "display_name", spec.family);
    spec.category = json_str(j, "category");
    spec.description = json_str(j, "description");
    spec.status = json_str(j, "status");
    spec.tasks = json_str_array(j, "tasks");
    spec.modes = json_str_array(j, "modes");
    spec.languages = json_str_array(j, "languages");

    // package_defaults.download.{repo,revision,gated}. May be null/absent —
    // then the download info lives on the individual packages (T5 reads those
    // per-package overrides when it needs them).
    const auto pd = j.find("package_defaults");
    if (pd != j.end() && pd->is_object()) {
        const auto dl = pd->find("download");
        if (dl != pd->end() && dl->is_object()) {
            spec.repo = json_str(*dl, "repo");
            spec.revision = json_str(*dl, "revision");
            spec.gated = dl->value("gated", false);
        }
    }

    // packages[] — historically some specs mixed plain-string entries in with
    // the package objects; skip non-object entries defensively.
    const auto pkgs = j.find("packages");
    if (pkgs == j.end() || !pkgs->is_array()) {
        return spec;
    }
    for (const auto& p : *pkgs) {
        if (!p.is_object()) {
            std::cerr << "catalog: skipping non-object package entry in spec '"
                      << spec.family << "'\n";
            continue;
        }
        Package pkg;
        pkg.id = json_str(p, "id");
        if (pkg.id.empty()) {
            std::cerr << "catalog: skipping package without 'id' in spec '"
                      << spec.family << "'\n";
            continue;
        }
        pkg.precision = json_str(p, "precision");
        pkg.format = json_str(p, "format");
        pkg.target_directory = json_str(p, "target_directory");
        pkg.files = json_str_array(p, "files");
        pkg.strip_prefix = json_str(p, "strip_prefix");
        // "default" may be absent or null — value() falls back for both.
        pkg.is_default = p.value("default", false);
        // Per-package download override (safetensors packages, gated repos).
        const auto dl = p.find("download");
        if (dl != p.end() && dl->is_object()) {
            Package::Download d;
            d.repo = json_str(*dl, "repo");
            d.revision = json_str(*dl, "revision", "main");
            d.gated = dl->value("gated", false);
            pkg.download = std::move(d);
        }
        spec.packages.push_back(std::move(pkg));
    }
    return spec;
}

}  // namespace

std::vector<Spec> load_catalog(const fs::path& specs_dir) {
    std::vector<Spec> specs;

    std::error_code ec;
    fs::directory_iterator it(specs_dir, ec);
    if (ec) {
        std::cerr << "catalog: cannot open specs dir '" << specs_dir << "': "
                  << ec.message() << "\n";
        return specs;
    }

    std::vector<fs::path> files;
    for (const auto& entry : it) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());  // deterministic order

    for (const auto& path : files) {
        std::ifstream in(path);
        if (!in) {
            std::cerr << "catalog: cannot read '" << path << "'\n";
            continue;
        }
        try {
            nlohmann::json j;
            in >> j;
            Spec spec = parse_spec(j);
            if (spec.family.empty()) {
                std::cerr << "catalog: skipping '" << path.filename().string()
                          << "': missing 'family'\n";
                continue;
            }
            specs.push_back(std::move(spec));
        } catch (const std::exception& ex) {
            std::cerr << "catalog: skipping malformed '" << path.filename().string()
                      << "': " << ex.what() << "\n";
        }
    }
    return specs;
}

const Spec* find_spec(const std::vector<Spec>& specs, std::string_view family) {
    for (const auto& spec : specs) {
        if (spec.family == family) {
            return &spec;
        }
    }
    return nullptr;
}

}  // namespace persona
