#include "config.h"
#include "model/catalog.h"
#include "model/download.h"

#include "engine/framework/runtime/registry.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace persona {

namespace fs = std::filesystem;

// persona models — model catalog verbs (Decision 9: the shipped
// model_specs/*.json are the catalog; no live HF search).
//
// Exit codes: 0 success, 1 user error / empty result, 2 internal error
// (e.g. catalog dir unreadable).

namespace {

// ---- text helpers ----------------------------------------------------------

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool contains_ci(const std::string& haystack, const std::string& needle) {
    return to_lower(haystack).find(to_lower(needle)) != std::string::npos;
}

std::string pad(const std::string& s, size_t width) {
    return s.size() >= width ? s : s + std::string(width - s.size(), ' ');
}

// Joins a list for table/info output, truncating at max_items with a "+N more"
// suffix (nemotron_asr has 40 language codes; qwen3_asr has 31).
std::string join_trunc(const std::vector<std::string>& items, size_t max_items) {
    std::string out;
    const size_t shown = std::min(items.size(), max_items);
    for (size_t i = 0; i < shown; ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += items[i];
    }
    if (items.size() > shown) {
        out += ", ... +" + std::to_string(items.size() - shown) + " more";
    }
    return out;
}

std::string join_all(const std::vector<std::string>& items) {
    return join_trunc(items, items.size());
}

// ---- spec helpers ----------------------------------------------------------

// The package to advertise as default: the one flagged is_default, else the
// first package (mirrors the T3 registry fallback). Nullptr if the spec has no
// usable packages.
const Package* default_package(const Spec& spec) {
    for (const auto& pkg : spec.packages) {
        if (pkg.is_default) {
            return &pkg;
        }
    }
    return spec.packages.empty() ? nullptr : &spec.packages[0];
}

// Directory under models_root that a spec's default package installs into.
// Same resolution as the T3 registry's resolve_asr_model_dir.
fs::path install_dir_for(const Spec& spec, const fs::path& models_root) {
    if (const Package* pkg = default_package(spec);
        pkg != nullptr && !pkg->target_directory.empty()) {
        return models_root / pkg->target_directory;
    }
    return models_root / spec.family;
}

std::string primary_task(const Spec& spec) {
    return spec.tasks.empty() ? "-" : spec.tasks[0];
}

std::string modes_str(const Spec& spec) {
    if (spec.modes.empty()) {
        return "-";
    }
    return join_all(spec.modes);
}

// du -sb equivalent (apparent size in bytes) over a directory tree.
uintmax_t dir_size(const fs::path& dir) {
    uintmax_t total = 0;
    std::error_code ec;
    fs::recursive_directory_iterator it(dir,
        fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        if (it->is_regular_file(ec)) {
            total += it->file_size(ec);
        }
        it.increment(ec);
    }
    return total;
}

std::string human_size(uintmax_t bytes) {
    static const char* kUnits[] = {"B", "K", "M", "G", "T"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    std::ostringstream out;
    if (u == 0) {
        out << static_cast<uintmax_t>(v) << kUnits[u];
    } else {
        out << std::fixed << std::setprecision(1) << v << kUnits[u];
    }
    return out.str();
}

// ---- subcommand usage ------------------------------------------------------

void print_models_usage(std::ostream& os) {
    os <<
        "Model catalog commands (the shipped model_specs/*.json are the catalog):\n"
        "\n"
        "  persona models search [--task <asr|tts|vad|...>] [--streaming]\n"
        "                       [--lang <code>] [--q <substr>]\n"
        "      Search the catalog; filters combine (AND). Empty result exits 1.\n"
        "  persona models list\n"
        "      Per family: installed? (models root), on-disk size, and whether\n"
        "      this binary's engine has a compiled-in loader (LOADER column).\n"
        "  persona models info <family>\n"
        "      Full spec: tasks/modes/languages, download repo/revision, packages.\n"
        "  persona models install <family> [--package <id>] [--force]\n"
        "      Download a family's package from Hugging Face (libcurl, .part resume).\n"
        "      Default: the spec's default package; --package picks a specific variant.\n"
        "  persona models uninstall <family>\n"
        "      Remove the family's installed files and manifest.\n";
}

// ---- search ----------------------------------------------------------------

// persona models search [--task <v>] [--streaming] [--lang <code>] [--q <s>]
int verb_search(const Config& cfg, const std::vector<std::string>& args) {
    std::string task, lang, q;
    bool streaming = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto need_value = [&](const char* name) -> std::string {
            if (i + 1 >= args.size()) {
                throw std::runtime_error(std::string("--") + name + " requires a value");
            }
            return args[++i];
        };
        if (a == "--task") {
            task = need_value("task");
        } else if (a == "--streaming") {
            streaming = true;
        } else if (a == "--lang") {
            lang = need_value("lang");
        } else if (a == "--q") {
            q = need_value("q");
        } else if (a == "--help" || a == "-h") {
            print_models_usage(std::cout);
            return 0;
        } else {
            std::cerr << "models search: unknown option '" << a << "'\n";
            return 1;
        }
    }

    const std::vector<Spec> specs = load_catalog(cfg.specs_dir);
    if (specs.empty()) {
        std::cerr << "models search: no model specs found in '" << cfg.specs_dir << "'\n";
        return 2;
    }

    const std::string task_l = to_lower(task);
    const std::string lang_l = to_lower(lang);
    const std::string q_l = to_lower(q);

    std::vector<const Spec*> matches;
    for (const Spec& spec : specs) {
        if (!task_l.empty()) {
            const bool has = std::any_of(
                spec.tasks.begin(), spec.tasks.end(),
                [&](const std::string& t) { return to_lower(t) == task_l; });
            if (!has) {
                continue;
            }
        }
        if (streaming) {
            const bool has = std::find(spec.modes.begin(), spec.modes.end(),
                                       "streaming") != spec.modes.end();
            if (!has) {
                continue;
            }
        }
        if (!lang_l.empty()) {
            // Case-insensitive prefix match on the language code ("en" matches
            // "en", "en-US", "english").
            const bool has = std::any_of(
                spec.languages.begin(), spec.languages.end(),
                [&](const std::string& l) {
                    const std::string l_l = to_lower(l);
                    return l_l.compare(0, lang_l.size(), lang_l) == 0;
                });
            if (!has) {
                continue;
            }
        }
        if (!q_l.empty()) {
            if (!contains_ci(spec.family, q_l) &&
                !contains_ci(spec.display_name, q_l) &&
                !contains_ci(spec.description, q_l)) {
                continue;
            }
        }
        matches.push_back(&spec);
    }

    if (matches.empty()) {
        std::cerr << "models search: no models match the given filters\n"
                  << "  try: persona models search  (no filters) or --q <name>\n";
        return 1;
    }

    std::sort(matches.begin(), matches.end(),
              [](const Spec* a, const Spec* b) { return a->family < b->family; });

    // Aligned columns: FAMILY  DISPLAY  TASK  MODES  DEFAULT-PACKAGE(precision)
    size_t w_family = std::string("FAMILY").size();
    size_t w_display = std::string("DISPLAY").size();
    size_t w_task = std::string("TASK").size();
    size_t w_modes = std::string("MODES").size();
    for (const Spec* s : matches) {
        w_family = std::max(w_family, s->family.size());
        w_display = std::max(w_display, s->display_name.size());
        w_task = std::max(w_task, primary_task(*s).size());
        w_modes = std::max(w_modes, modes_str(*s).size());
    }

    auto print_row = [&](const std::string& family, const std::string& display,
                         const std::string& task_col, const std::string& modes_col,
                         const std::string& pkg_col) {
        std::cout << pad(family, w_family) << "  " << pad(display, w_display)
                  << "  " << pad(task_col, w_task) << "  " << pad(modes_col, w_modes)
                  << "  " << pkg_col << "\n";
    };

    print_row("FAMILY", "DISPLAY", "TASK", "MODES", "DEFAULT-PACKAGE(precision)");
    for (const Spec* s : matches) {
        std::string pkg_col = "-";
        if (const Package* pkg = default_package(*s); pkg != nullptr) {
            pkg_col = pkg->id + "(" + pkg->precision + ")";
        }
        print_row(s->family, s->display_name, primary_task(*s), modes_str(*s), pkg_col);
    }
    return 0;
}

// ---- list ------------------------------------------------------------------

// persona models list — per family: installed?, on-disk size (default
// package's target_directory under the models root, same resolution as the
// T3 registry), and whether THIS binary's engine has a compiled-in loader
// for the family. "--models-root" is a global flag, already consumed by
// parse_args, so `persona models list --models-root <dir>` just works.
int verb_list(const Config& cfg, const std::vector<std::string>& args) {
    if (!args.empty()) {
        std::cerr << "models list: unexpected argument '" << args[0] << "'\n"
                  << "  usage: persona models list\n";
        return 1;
    }

    const std::vector<Spec> specs = load_catalog(cfg.specs_dir);
    if (specs.empty()) {
        std::cerr << "models list: no model specs found in '" << cfg.specs_dir << "'\n";
        return 2;
    }

    // Compiled-in loader families: the engine loaders THIS binary was built
    // with (-DAUDIOCPP_MODELS in flake.nix). A family can be installed (files
    // on disk) yet unloadable here — the composite build only compiles a
    // subset of loaders, so "INSTALLED yes" does NOT imply the daemon can
    // load it. The LOADER column makes the gap visible: yes = this binary can
    // load the family, - = it cannot (rebuild required, T15).
    engine::runtime::ModelRegistry registry = engine::runtime::make_default_registry();
    std::vector<std::string> loader_families;
    for (const auto& loader : registry.advertise_loaders()) {
        loader_families.push_back(loader.family);
    }
    const auto has_loader = [&](const std::string& family) {
        return std::find(loader_families.begin(), loader_families.end(), family) !=
               loader_families.end();
    };

    std::vector<const Spec*> sorted;
    sorted.reserve(specs.size());
    for (const Spec& s : specs) {
        sorted.push_back(&s);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const Spec* a, const Spec* b) { return a->family < b->family; });

    // Collect rows first so the SIZE column can be aligned with the new
    // LOADER column (the old output's trailing SIZE column needed no width).
    struct Row {
        const Spec* spec;
        bool installed;
        std::string size_str;
        bool loader;
    };
    std::vector<Row> rows;
    rows.reserve(sorted.size());
    for (const Spec* s : sorted) {
        // Prefer the T5 manifest (authoritative bytes, works offline); fall
        // back to du -sb of the default package's target dir.
        uintmax_t size = 0;
        bool installed = false;
        if (const auto m = read_manifest(cfg.models_root, s->family); m) {
            for (const auto& [path, bytes] : m->files) {
                (void)path;
                size += bytes;
            }
            if (!m->files.empty()) {
                std::error_code ec;
                installed = fs::is_regular_file(
                    fs::path(cfg.models_root) / m->files[0].first, ec);
            }
        }
        if (!installed) {
            const fs::path dir = install_dir_for(*s, cfg.models_root);
            std::error_code ec;
            installed = fs::is_directory(dir, ec);
            size = installed ? dir_size(dir) : 0;
        }
        rows.push_back({s, installed, installed ? human_size(size) : "-",
                        has_loader(s->family)});
    }

    size_t w_family = std::string("FAMILY").size();
    size_t w_size = std::string("SIZE").size();
    for (const Row& r : rows) {
        w_family = std::max(w_family, r.spec->family.size());
        w_size = std::max(w_size, r.size_str.size());
    }

    std::cout << pad("FAMILY", w_family) << "  INSTALLED  "
              << pad("SIZE", w_size) << "  LOADER\n";
    for (const Row& r : rows) {
        std::cout << pad(r.spec->family, w_family) << "  "
                  << pad(r.installed ? "yes" : "no", 9) << "  "
                  << pad(r.size_str, w_size) << "  "
                  << pad(r.loader ? "yes" : "-", 6) << "\n";
    }
    return 0;
}

// ---- info ------------------------------------------------------------------

// persona models info <family>
int verb_info(const Config& cfg, const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cerr << "models info: expected exactly one family argument\n"
                  << "  usage: persona models info <family>\n";
        return 1;
    }
    const std::string family = args[0];  // lookup key only — never a path

    const std::vector<Spec> specs = load_catalog(cfg.specs_dir);
    const Spec* spec = find_spec(specs, family);
    if (spec == nullptr) {
        std::cerr << "models info: unknown model family '" << family << "'\n"
                  << "  try: persona models search --q " << family << "\n";
        return 1;
    }

    std::cout << spec->family << " - " << spec->display_name << "\n";
    std::cout << "  category:   " << (spec->category.empty() ? "-" : spec->category) << "\n";
    std::cout << "  status:     " << (spec->status.empty() ? "-" : spec->status) << "\n";
    if (!spec->description.empty()) {
        std::cout << "  description: " << spec->description << "\n";
    }
    std::cout << "  tasks:      " << (spec->tasks.empty() ? "-" : join_all(spec->tasks)) << "\n";
    std::cout << "  modes:      " << (spec->modes.empty() ? "-" : join_all(spec->modes)) << "\n";
    std::cout << "  languages:  " << (spec->languages.empty() ? "-" : join_trunc(spec->languages, 8)) << "\n";

    if (spec->repo) {
        std::cout << "  download:   " << *spec->repo
                  << (spec->revision ? " @ " + *spec->revision : "") << "\n";
        if (spec->gated) {
            std::cout << "  warning: gated repository - install needs HUGGING_FACE_HUB_TOKEN\n";
        }
    } else {
        std::cout << "  download:   (per-package - see packages below)\n";
    }

    std::cout << "  packages (" << spec->packages.size() << "):\n";
    for (const Package& pkg : spec->packages) {
        std::cout << "    " << (pkg.is_default ? "[default] " : "          ")
                  << pkg.id << "  " << pkg.precision << "  " << pkg.format << "\n";
    }
    return 0;
}

// ---- install / uninstall (T5) ---------------------------------------------

// persona models install <family> [--package <id>] [--force]
int verb_install(const Config& cfg, const std::vector<std::string>& args) {
    std::string family, package_id;
    bool force = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--package") {
            if (i + 1 >= args.size()) {
                throw std::runtime_error("--package requires a value");
            }
            package_id = args[++i];
        } else if (a == "--force") {
            force = true;
        } else if (a == "--help" || a == "-h") {
            print_models_usage(std::cout);
            return 0;
        } else if (family.empty()) {
            family = a;
        } else {
            std::cerr << "models install: unexpected argument '" << a << "'\n"
                      << "  usage: persona models install <family> [--package <id>]\n";
            return 1;
        }
    }
    if (family.empty()) {
        std::cerr << "models install: missing family argument\n"
                  << "  usage: persona models install <family> [--package <id>]\n";
        return 1;
    }

    const std::vector<Spec> specs = load_catalog(cfg.specs_dir);
    const Spec* spec = find_spec(specs, family);
    if (spec == nullptr) {
        std::cerr << "models install: unknown model family '" << family << "'\n"
                  << "  try: persona models search --q " << family << "\n";
        return 1;
    }

    // Package selection: --package wins (validated against the catalog), else
    // the spec's default ("default":true, or the first package).
    const Package* pkg = nullptr;
    if (!package_id.empty()) {
        for (const Package& p : spec->packages) {
            if (p.id == package_id) {
                pkg = &p;
                break;
            }
        }
        if (pkg == nullptr) {
            std::cerr << "models install: unknown package '" << package_id
                      << "' for family '" << family << "'\n"
                      << "  try: persona models info " << family << "\n";
            return 1;
        }
    } else {
        pkg = default_package(*spec);
        if (pkg == nullptr) {
            std::cerr << "models install: family '" << family
                      << "' has no installable packages\n";
            return 2;
        }
    }

    InstallResult res = install_package(*spec, *pkg, cfg.models_root, force);
    if (!res.ok) {
        std::cerr << "models install: " << res.error << "\n";
        return 2;
    }

    // Machine-readable single line on stdout (progress went to stderr).
    std::cout << "installed " << family << " " << pkg->id << " ("
              << res.total_bytes << ")\n";
    return 0;
}

// persona models uninstall <family>
int verb_uninstall(const Config& cfg, const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cerr << "models uninstall: expected exactly one family argument\n"
                  << "  usage: persona models uninstall <family>\n";
        return 1;
    }
    const std::string family = args[0];

    const std::vector<Spec> specs = load_catalog(cfg.specs_dir);
    const Spec* spec = find_spec(specs, family);
    if (spec == nullptr) {
        std::cerr << "models uninstall: unknown model family '" << family << "'\n"
                  << "  try: persona models search --q " << family << "\n";
        return 1;
    }

    std::string error;
    if (!uninstall_family(*spec, cfg.models_root, error)) {
        std::cerr << "models uninstall: " << error << "\n";
        return 2;
    }
    std::cout << "uninstalled " << family << "\n";
    return 0;
}

}  // namespace

// persona models <search|list|info|install|uninstall> (install/uninstall = T5).
int verb_models(const Config& cfg, const std::vector<std::string>& args) {
    if (args.empty()) {
        print_models_usage(std::cerr);
        return 1;
    }
    const std::string& sub = args[0];
    const std::vector<std::string> rest(args.begin() + 1, args.end());

    if (sub == "search") {
        return verb_search(cfg, rest);
    }
    if (sub == "list") {
        return verb_list(cfg, rest);
    }
    if (sub == "info") {
        return verb_info(cfg, rest);
    }
    if (sub == "install") {
        return verb_install(cfg, rest);
    }
    if (sub == "uninstall") {
        return verb_uninstall(cfg, rest);
    }
    std::cerr << "models: unknown subcommand '" << sub << "'\n\n";
    print_models_usage(std::cerr);
    return 1;
}

}  // namespace persona
