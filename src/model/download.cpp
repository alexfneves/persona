// HF model downloader (T5): spec-driven package installs via libcurl.
//
// URL scheme mirrors audio.cpp's tools/model_manager_v2.py hf_url():
//   https://huggingface.co/<repo>/resolve/<revision>/<file>
// with every path segment percent-quoted (urllib.quote safe='' semantics:
// only [A-Za-z0-9._~-] survive unescaped). Files land at
// <models_root>/<target_directory>/<strip_prefix applied>, exactly like the
// reference tool's stripped_path().
//
// No python/torch at runtime — libcurl only (Decision 6).

#include "model/download.h"

#include <nlohmann/json.hpp>

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>

namespace persona {

namespace fs = std::filesystem;

namespace {

// ---- small text helpers ----------------------------------------------------

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return s.substr(b, e - b);
}

// 1234567890 -> "1.2G". Duplicated from models.cpp (both live in anonymous
// namespaces; a shared util header would be ceremony for 10 lines).
std::string human_bytes(uintmax_t bytes) {
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

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.push_back(cur);
    return parts;
}

std::string iso_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// ---- URL construction (mirrors model_manager_v2.py) ------------------------

// urllib.quote(part, safe='') equivalent: percent-encode everything outside
// the unreserved set [A-Za-z0-9._~-].
std::string quote_segment(std::string_view seg) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(seg.size());
    for (const unsigned char c : seg) {
        if (std::isalnum(c) || c == '.' || c == '_' || c == '~' || c == '-') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0xF];
        }
    }
    return out;
}

std::string hf_url(const std::string& repo, const std::string& revision,
                   const std::string& remote_file) {
    std::string url = "https://huggingface.co/";
    bool first = true;
    for (const auto& seg : split(repo, '/')) {
        if (!first) {
            url += '/';
        }
        url += quote_segment(seg);
        first = false;
    }
    url += "/resolve/";
    url += quote_segment(revision);
    url += '/';
    first = true;
    for (const auto& seg : split(remote_file, '/')) {
        if (!first) {
            url += '/';
        }
        url += quote_segment(seg);
        first = false;
    }
    return url;
}

// Applies a package's strip_prefix to a remote path (model_manager_v2.py's
// stripped_path()). Caller guarantees the prefix actually prefixes the path.
std::string stripped_path(const std::string& remote_file, const std::string& strip_prefix) {
    if (strip_prefix.empty()) {
        return remote_file;
    }
    std::string prefix = strip_prefix;
    while (!prefix.empty() && prefix.back() == '/') {
        prefix.pop_back();
    }
    return remote_file.substr(prefix.size() + 1);  // skip "prefix/"
}

// A relative path may not be absolute, may not be ".", and may not contain
// ".." components — install targets and uninstall targets must stay inside
// the models root.
bool is_safe_relative(const fs::path& p) {
    if (p.empty() || p.is_absolute() || p == ".") {
        return false;
    }
    for (const auto& part : p) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

// ---- download info resolution ---------------------------------------------

// Per-package download info: the package-level `download` override wins, else
// the spec's package_defaults.download (Spec::repo/revision/gated).
struct DownloadInfo {
    std::string repo;
    std::string revision = "main";
    bool gated = false;
};

DownloadInfo resolve_download(const Spec& spec, const Package& pkg) {
    DownloadInfo info;
    if (pkg.download) {
        info.repo = pkg.download->repo;
        info.revision = pkg.download->revision.empty() ? "main" : pkg.download->revision;
        info.gated = pkg.download->gated;
    } else {
        info.repo = spec.repo.value_or("");
        info.revision = spec.revision.value_or("main");
        info.gated = spec.gated;
    }
    return info;
}

// HF auth token from the environment (same sources as model_manager_v2.py,
// minus the ~/.cache file — persona is a CLI, not an interactive tool).
std::string hf_token() {
    const char* t = std::getenv("HF_TOKEN");
    if (t == nullptr) {
        t = std::getenv("HUGGING_FACE_HUB_TOKEN");
    }
    if (t == nullptr) {
        return {};
    }
    return trim(t);
}

// ---- libcurl RAII ----------------------------------------------------------

// curl_global_init once per process.
void ensure_curl_global() {
    static const bool initialized = [] {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        return true;
    }();
    (void)initialized;
}

struct CurlHandle {
    CURL* raw = nullptr;
    CurlHandle() {
        ensure_curl_global();
        raw = curl_easy_init();
    }
    ~CurlHandle() {
        if (raw != nullptr) {
            curl_easy_cleanup(raw);
        }
    }
    explicit operator bool() const { return raw != nullptr; }
};

struct HeaderList {
    curl_slist* raw = nullptr;
    HeaderList() = default;
    HeaderList(const HeaderList&) = delete;
    HeaderList& operator=(const HeaderList&) = delete;
    HeaderList(HeaderList&& o) noexcept : raw(o.raw) { o.raw = nullptr; }
    HeaderList& operator=(HeaderList&& o) noexcept {
        if (this != &o) {
            if (raw != nullptr) {
                curl_slist_free_all(raw);
            }
            raw = o.raw;
            o.raw = nullptr;
        }
        return *this;
    }
    ~HeaderList() {
        if (raw != nullptr) {
            curl_slist_free_all(raw);
        }
    }
};

// Common options for every request. The Authorization header is attached
// whenever a token is set (gated or not — sending it on public repos is
// harmless, and it is *required* for gated ones).
HeaderList apply_common_opts(CURL* curl, const std::string& url, const std::string& token) {
    HeaderList headers;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "persona/0.1.0");
    if (!token.empty()) {
        headers.raw = curl_slist_append(headers.raw,
                                        ("Authorization: Bearer " + token).c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.raw);
    }
    return headers;
}

// Remote file size via HEAD (nullopt on any error). Used to skip
// already-downloaded files when no manifest exists yet.
std::optional<uintmax_t> remote_size(const std::string& url, const std::string& token) {
    CurlHandle curl;
    if (!curl) {
        return std::nullopt;
    }
    HeaderList headers = apply_common_opts(curl.raw, url, token);
    curl_easy_setopt(curl.raw, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl.raw, CURLOPT_TIMEOUT, 60L);
    const CURLcode rc = curl_easy_perform(curl.raw);
    if (rc != CURLE_OK) {
        return std::nullopt;
    }
    curl_off_t size = -1;
    curl_easy_getinfo(curl.raw, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &size);
    if (size < 0) {
        return std::nullopt;
    }
    return static_cast<uintmax_t>(size);
}

// ---- single-file download with resume --------------------------------------

struct ProgressCtx {
    FILE* fh = nullptr;
    std::string name;       // stderr display name (repo/remote path)
    uintmax_t base = 0;     // bytes already in the .part (resume offset)
    uintmax_t written = 0;  // bytes written this transfer
    uintmax_t last_report = 0;
};

size_t progress_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<ProgressCtx*>(userdata);
    const size_t n = std::fwrite(ptr, size, nmemb, ctx->fh);
    ctx->written += n;
    // Progress to stderr every ~8 MiB of new data.
    if (ctx->written + ctx->base - ctx->last_report >= (8u << 20)) {
        std::fprintf(stderr, "\r  %s: %s", ctx->name.c_str(),
                     human_bytes(ctx->written + ctx->base).c_str());
        ctx->last_report = ctx->written + ctx->base;
    }
    return n;
}

struct DownloadOutcome {
    enum class Kind { Ok, AlreadyComplete, Failed };
    Kind kind = Kind::Failed;
    uintmax_t bytes = 0;  // bytes on disk after (Ok / AlreadyComplete)
    std::string error;
};

// Downloads url into part_path (resuming from its current size), renaming to
// final_path on success. On failure the .part is left in place for a later
// resume. `display_name` feeds the stderr progress line.
DownloadOutcome download_file(const std::string& url, const std::string& token,
                              const fs::path& part_path, const fs::path& final_path,
                              const std::string& display_name) {
    std::error_code ec;
    fs::create_directories(part_path.parent_path(), ec);

    uintmax_t existing = 0;
    if (fs::exists(part_path, ec)) {
        existing = fs::file_size(part_path, ec);
        if (ec) {
            existing = 0;
        }
    }

    CurlHandle curl;
    if (!curl) {
        return {DownloadOutcome::Kind::Failed, 0, "libcurl init failed"};
    }

    ProgressCtx ctx;
    ctx.fh = std::fopen(part_path.c_str(), "ab");  // append == write at resume offset
    if (ctx.fh == nullptr) {
        return {DownloadOutcome::Kind::Failed, 0,
                "cannot open " + part_path.string()};
    }
    ctx.name = display_name;
    ctx.base = existing;
    ctx.last_report = existing;

    HeaderList headers = apply_common_opts(curl.raw, url, token);
    curl_easy_setopt(curl.raw, CURLOPT_WRITEFUNCTION, progress_write_cb);
    curl_easy_setopt(curl.raw, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl.raw, CURLOPT_NOPROGRESS, 1L);
    if (existing > 0) {
        curl_easy_setopt(curl.raw, CURLOPT_RESUME_FROM_LARGE,
                         static_cast<curl_off_t>(existing));
    }
    // Stall watchdog: abort if the connection delivers < 1 byte/s for 60 s.
    curl_easy_setopt(curl.raw, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl.raw, CURLOPT_LOW_SPEED_LIMIT, 1L);

    const CURLcode rc = curl_easy_perform(curl.raw);
    long status = 0;
    curl_easy_getinfo(curl.raw, CURLINFO_RESPONSE_CODE, &status);
    curl_off_t content_length = -1;
    curl_easy_getinfo(curl.raw, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &content_length);
    const uintmax_t written = ctx.written;
    std::fclose(ctx.fh);
    ctx.fh = nullptr;
    std::fprintf(stderr, "\r  %s: %s\n", display_name.c_str(),
                 human_bytes(existing + written).c_str());

    // 416 = range unsatisfiable: the .part already holds the whole file.
    if (existing > 0 && status == 416) {
        const auto rs = remote_size(url, token);
        if (rs && *rs == existing) {
            fs::rename(part_path, final_path, ec);
            if (!ec) {
                return {DownloadOutcome::Kind::AlreadyComplete, existing, {}};
            }
        }
        return {DownloadOutcome::Kind::Failed, 0,
                "stale .part (server reports a different size); remove " +
                    part_path.string()};
    }

    if (rc != CURLE_OK) {
        std::string err = curl_easy_strerror(rc);
        if (status != 0) {
            err += " (HTTP " + std::to_string(status) + ")";
        }
        return {DownloadOutcome::Kind::Failed, 0, err};
    }

    // The server ignored the resume request (200 instead of 206): data would
    // be appended after stale bytes, corrupting the file.
    if (existing > 0 && status == 200) {
        return {DownloadOutcome::Kind::Failed, 0,
                "server ignored resume request; remove " + part_path.string()};
    }

    // Size verification (HF gives no checksums — trust the byte count, per the
    // plan's risk register).
    if (content_length >= 0 &&
        written != static_cast<uintmax_t>(content_length)) {
        return {DownloadOutcome::Kind::Failed, 0,
                "size mismatch: got " + std::to_string(written) +
                    " bytes, expected " + std::to_string(content_length)};
    }

    fs::rename(part_path, final_path, ec);
    if (ec) {
        return {DownloadOutcome::Kind::Failed, 0,
                "cannot rename " + part_path.string() + ": " + ec.message()};
    }
    return {DownloadOutcome::Kind::Ok, existing + written, {}};
}

}  // namespace

// ---- public API ------------------------------------------------------------

InstallResult install_package(const Spec& spec, const Package& pkg,
                              const fs::path& models_root, bool force) {
    InstallResult result;

    const DownloadInfo dl = resolve_download(spec, pkg);
    if (dl.repo.empty()) {
        result.error = "no download source in spec for package '" + pkg.id +
                       "' (family '" + spec.family + "')";
        return result;
    }
    if (!is_safe_relative(fs::path(pkg.target_directory))) {
        result.error = "unsafe target_directory '" + pkg.target_directory +
                       "' in spec for family '" + spec.family + "'";
        return result;
    }

    const std::string token = hf_token();

    // Local layout: <models_root>/<target_directory>/<stripped file>. Manifest
    // paths are recorded relative to models_root.
    std::vector<std::pair<std::string, std::string>> plan;  // (remote, rel)
    for (const std::string& remote : pkg.files) {
        const std::string rel =
            (fs::path(pkg.target_directory) / stripped_path(remote, pkg.strip_prefix))
                .string();
        if (!is_safe_relative(fs::path(rel))) {
            result.error = "unsafe target path '" + rel + "' for family '" +
                           spec.family + "'";
            return result;
        }
        plan.emplace_back(remote, rel);
    }

    // Fast path: the manifest already records this exact package and every
    // file is present with the recorded size -> nothing to do.
    const auto manifest = read_manifest(models_root, spec.family);
    if (!force && manifest && manifest->package == pkg.id) {
        bool up_to_date = true;
        for (const auto& [remote, rel] : plan) {
            (void)remote;
            std::error_code ec;
            uintmax_t recorded = 0;
            for (const auto& [mp, mb] : manifest->files) {
                if (mp == rel) {
                    recorded = mb;
                    break;
                }
            }
            if (!fs::is_regular_file(models_root / rel, ec) ||
                fs::file_size(models_root / rel, ec) != recorded) {
                up_to_date = false;
                break;
            }
        }
        if (up_to_date) {
            for (const auto& [path, bytes] : manifest->files) {
                (void)path;
                result.total_bytes += bytes;
            }
            result.ok = true;
            return result;
        }
    }

    // Per-file download: skip files already on disk with a matching size,
    // otherwise one attempt + one retry (resuming via .part).
    for (const auto& [remote, rel] : plan) {
        const fs::path final_path = models_root / rel;
        const fs::path part_path = fs::path(final_path.string() + ".part");
        const std::string url = hf_url(dl.repo, dl.revision, remote);
        const std::string display = dl.repo + "/" + remote;

        std::error_code ec;
        if (!force && fs::is_regular_file(final_path, ec)) {
            const uintmax_t have = fs::file_size(final_path, ec);
            bool match = false;
            if (manifest) {
                for (const auto& [mp, mb] : manifest->files) {
                    if (mp == rel && mb == have) {
                        match = true;
                        break;
                    }
                }
            }
            if (!match) {
                if (auto rs = remote_size(url, token)) {
                    match = *rs == have;
                }
            }
            if (match) {
                result.total_bytes += have;
                continue;
            }
        }
        if (force) {
            fs::remove(final_path, ec);
            fs::remove(part_path, ec);
        }

        std::string last_error;
        bool done = false;
        for (int attempt = 0; attempt < 2 && !done; ++attempt) {
            std::fprintf(stderr, "downloading %s%s\n", display.c_str(),
                         attempt > 0 ? " (retry)" : "");
            const DownloadOutcome out =
                download_file(url, token, part_path, final_path, display);
            switch (out.kind) {
                case DownloadOutcome::Kind::Ok:
                case DownloadOutcome::Kind::AlreadyComplete:
                    result.total_bytes += out.bytes;
                    done = true;
                    break;
                case DownloadOutcome::Kind::Failed:
                    last_error = out.error;
                    break;
            }
        }
        if (!done) {
            result.error = "failed to download '" + display + "': " + last_error +
                           " (.part kept for resume)";
            return result;
        }
    }

    // All files present: write the family manifest.
    std::error_code ec;
    fs::create_directories(models_root / spec.family, ec);
    if (ec) {
        result.error = "cannot create '" + (models_root / spec.family).string() +
                       "': " + ec.message();
        return result;
    }
    nlohmann::json j;
    j["family"] = spec.family;
    j["package"] = pkg.id;
    j["installed_at"] = iso_now();
    j["repo"] = dl.repo;
    j["revision"] = dl.revision;
    j["files"] = nlohmann::json::array();
    for (const auto& [remote, rel] : plan) {
        (void)remote;
        std::error_code fec;
        const uintmax_t bytes = fs::file_size(models_root / rel, fec);
        j["files"].push_back({{"path", rel}, {"bytes", bytes}});
    }
    const fs::path manifest_path =
        models_root / spec.family / ".persona-manifest.json";
    std::ofstream out(manifest_path);
    if (!out) {
        result.error = "cannot write " + manifest_path.string();
        return result;
    }
    out << j.dump(2) << "\n";
    out.close();
    if (!out) {
        result.error = "failed writing " + manifest_path.string();
        return result;
    }

    result.ok = true;
    return result;
}

bool uninstall_family(const Spec& spec, const fs::path& models_root,
                      std::string& error) {
    // Directory to remove: from the manifest (the last installed package), else
    // the default package's target_directory.
    fs::path target_dir;
    const auto manifest = read_manifest(models_root, spec.family);
    if (manifest && !manifest->files.empty()) {
        target_dir = fs::path(manifest->files[0].first).parent_path();
    }
    if (target_dir.empty()) {
        const Package* pkg = nullptr;
        for (const auto& p : spec.packages) {
            if (p.is_default) {
                pkg = &p;
                break;
            }
        }
        if (pkg == nullptr && !spec.packages.empty()) {
            pkg = &spec.packages[0];
        }
        if (pkg != nullptr) {
            target_dir = fs::path(pkg->target_directory);
        }
    }
    if (target_dir.empty()) {
        error = "cannot determine install directory for family '" + spec.family + "'";
        return false;
    }

    // Safety: the target must resolve strictly inside models_root.
    if (!is_safe_relative(target_dir)) {
        error = "refusing to remove unsafe path '" + target_dir.string() +
                "' (outside models root)";
        return false;
    }
    const fs::path full = (models_root / target_dir).lexically_normal();
    const fs::path root = models_root.lexically_normal();
    // Every root component must match the prefix of full (full is root or a
    // descendant). A non-matching root component means full escaped upward.
    const auto [it, end] = std::mismatch(root.begin(), root.end(), full.begin(), full.end());
    (void)end;
    if (it != root.end()) {
        error = "refusing to remove path outside models root: " + full.string();
        return false;
    }

    std::error_code ec;
    fs::remove_all(full, ec);
    if (ec) {
        error = "cannot remove '" + full.string() + "': " + ec.message();
        return false;
    }

    // Tidy up: remove now-empty ancestor directories (e.g. the parent of
    // "PocketTTS-GGUF/english"), stopping at the models root itself.
    for (fs::path dir = full.parent_path(); dir != root; dir = dir.parent_path()) {
        std::error_code lec;
        if (!fs::is_empty(dir, lec) || lec) {
            break;
        }
        fs::remove(dir, lec);
        if (lec) {
            break;
        }
    }

    // Manifest, then the (now possibly empty) family dir.
    fs::remove(models_root / spec.family / ".persona-manifest.json", ec);
    fs::remove(models_root / spec.family, ec);
    return true;
}

std::optional<FamilyManifest> read_manifest(const fs::path& models_root,
                                            const std::string& family) {
    std::ifstream in(models_root / family / ".persona-manifest.json");
    if (!in) {
        return std::nullopt;
    }
    try {
        nlohmann::json j;
        in >> j;
        if (!j.is_object() || !j.contains("files")) {
            return std::nullopt;
        }
        FamilyManifest m;
        m.family = j.value("family", family);
        m.package = j.value("package", "");
        m.repo = j.value("repo", "");
        m.revision = j.value("revision", "");
        m.installed_at = j.value("installed_at", "");
        for (const auto& f : j["files"]) {
            if (f.is_object() && f.contains("path") && f.contains("bytes")) {
                m.files.emplace_back(f["path"].get<std::string>(),
                                     f["bytes"].get<uintmax_t>());
            }
        }
        return m;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace persona
