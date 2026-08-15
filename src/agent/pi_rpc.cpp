#include "agent/pi_rpc.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <signal.h>
#include <sstream>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// environ for execve's envp (the child inherits persona's environment, e.g.
// HF_TOKEN / provider config).
extern char** environ;

namespace persona {

namespace {

// Upper bound on reply_buffer_ (v1: informational only — deltas beyond this
// are dropped). Keeps a long-running daemon's memory flat regardless of total
// reply volume (T9 review P2).
constexpr size_t kReplyBufferCap = 64 * 1024;

// Text of an AgentMessage (pi's message_end.message): concatenate the content
// blocks with type=="text" (skip thinking/toolCall blocks). Defensive: a
// string content (UserMessage shape) is used as-is; anything else yields "".
// Never throws, never crashes on unknown shapes — an empty reply is simply not
// spoken.
std::string extract_message_text(const nlohmann::json& message) {
    if (!message.is_object()) {
        return "";
    }
    const auto it = message.find("content");
    if (it == message.end()) {
        return "";
    }
    const nlohmann::json& content = *it;
    if (content.is_string()) {
        return content.get<std::string>();
    }
    if (content.is_array()) {
        std::string out;
        for (const auto& block : content) {
            if (block.is_object() && block.value("type", std::string()) == "text") {
                out += block.value("text", std::string());
            }
        }
        return out;
    }
    return "";
}

// execvp-style PATH resolution done in the PARENT (the forked child only calls
// async-signal-safe functions between fork and exec, so no malloc there). A
// name containing '/' is used as-is; otherwise each PATH entry is tried.
// Returns the resolved path (or the original name if not found — execve then
// fails with ENOENT).
std::string resolve_program(const std::string& name) {
    if (name.find('/') != std::string::npos) {
        return name;
    }
    const char* path_env = std::getenv("PATH");
    if (path_env == nullptr || *path_env == '\0') {
        return name;
    }
    std::istringstream entries(path_env);
    std::string dir;
    while (std::getline(entries, dir, ':')) {
        if (dir.empty()) {
            dir = ".";
        }
        const std::string candidate = dir + "/" + name;
        if (::access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
    }
    return name;
}

// Closes every fd >= 3 in the child before exec (only std fds are wanted: 0/1
// are the pipes, 2 is persona's stderr). Uses the close_range syscall when
// available, else a bounded loop. All async-signal-safe (no static state).
void close_fds_above_2() {
    const long r = ::syscall(SYS_close_range, 3u, ~0u, 0u);
    if (r == 0 || errno != ENOSYS) {
        return;
    }
    for (int fd = 3; fd < 1024; ++fd) {
        ::close(fd);
    }
}

// Writes a decimal integer to a fd (child-side diagnostics; snprintf is not
// async-signal-safe).
void write_uint_fd(int fd, unsigned v) {
    char buf[16];
    size_t n = 0;
    if (v == 0) {
        buf[n++] = '0';
    }
    while (v > 0) {
        buf[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    }
    while (n > 0) {
        const ssize_t unused = ::write(fd, &buf[--n], 1);
        (void)unused;
    }
}

}  // namespace

PiAgent::PiAgent(Events ev, std::string pi_bin, std::vector<std::string> extra_args)
    : ev_(std::move(ev)),
      pi_bin_(std::move(pi_bin)),
      extra_args_(std::move(extra_args)) {}

PiAgent::~PiAgent() {
    shutdown();
}

bool PiAgent::start() {
    if (started_) {
        return true;
    }

    int to_child[2] = {-1, -1};    // persona writes [1]; child reads [0] as stdin
    int from_child[2] = {-1, -1};  // child writes [1] as stdout; persona reads [0]
    if (::pipe(to_child) != 0) {
        fire_error(std::string("pipe(to_child): ") + std::strerror(errno));
        return false;
    }
    if (::pipe(from_child) != 0) {
        fire_error(std::string("pipe(from_child): ") + std::strerror(errno));
        ::close(to_child[0]);
        ::close(to_child[1]);
        return false;
    }

    // argv: pi --mode rpc [extra_args...]
    std::vector<char*> argv;
    argv.reserve(3 + extra_args_.size() + 1);
    argv.push_back(const_cast<char*>(pi_bin_.c_str()));
    argv.push_back(const_cast<char*>("--mode"));
    argv.push_back(const_cast<char*>("rpc"));
    for (const std::string& a : extra_args_) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);

    // Resolve the binary in the parent (PATH search), so the child never
    // mallocs between fork and exec. A missing binary is reported
    // synchronously here — the daemon then continues NDJSON-only.
    const std::string resolved = resolve_program(pi_bin_);
    if (::access(resolved.c_str(), X_OK) != 0) {
        ::close(to_child[0]);
        ::close(to_child[1]);
        ::close(from_child[0]);
        ::close(from_child[1]);
        std::string msg = "cannot exec '" + pi_bin_ + "': " + std::strerror(errno);
        if (errno == ENOENT) {
            msg += " (is pi installed on PATH? point PERSONA_PI_BIN at the binary)";
        }
        fire_error(std::move(msg));
        return false;
    }

    // fork + execve: reliable in a multithreaded process. (posix_spawnp's
    // internal vfork/clone races with the daemon's other threads — observed:
    // the child sometimes never exec'd, leaving the parent frozen in
    // sigsuspend, or the pipe dup2s silently dropped so the child inherited
    // /dev/null as stdin.)
    const pid_t pid = ::fork();
    if (pid < 0) {
        fire_error(std::string("fork: ") + std::strerror(errno));
        ::close(to_child[0]);
        ::close(to_child[1]);
        ::close(from_child[0]);
        ::close(from_child[1]);
        return false;
    }
    if (pid == 0) {
        // Child: only async-signal-safe calls here (the rest of the daemon's
        // threads are gone; the stdio/malloc locks they may have held are
        // stuck, so no libc allocation).
        ::dup2(to_child[0], 0);
        ::dup2(from_child[1], 1);
        close_fds_above_2();  // std fds 0/1 are the pipes; 2 is persona's stderr
        ::execve(resolved.c_str(), argv.data(), environ);
        // exec failed (ENOEXEC, EACCES, bad shebang, ...): report on stderr
        // (fd 2 survives close_range) so the failure is diagnosable.
        static const char msg[] = "pi_rpc: exec failed (errno ";
        const ssize_t unused = ::write(2, msg, sizeof(msg) - 1);
        (void)unused;
        write_uint_fd(2, static_cast<unsigned>(errno));
        static const char close_paren[] = ")\n";
        const ssize_t unused2 = ::write(2, close_paren, sizeof(close_paren) - 1);
        (void)unused2;
        _exit(127);
    }
    // Parent: drop the child's ends; keep to_child[1] (write) and
    // from_child[0] (read).
    ::close(to_child[0]);
    ::close(from_child[1]);

    pid_ = pid;
    write_fd_ = to_child[1];
    read_fd_ = from_child[0];
    started_ = true;
    shutting_down_ = false;
    eof_ = false;
    reply_buffer_.clear();
    if (std::getenv("PERSONA_DEBUG_TIMELINE")) {
        std::cerr << "dbg: pi pipes to_child={" << to_child[0] << "," << to_child[1]
                  << "} from_child={" << from_child[0] << "," << from_child[1]
                  << "} read_fd_=" << read_fd_ << " write_fd_=" << write_fd_ << "\n";
    }

    reader_ = std::thread([this] {
        std::string buf;
        char tmp[4096];
        for (;;) {
            const ssize_t n = ::read(read_fd_, tmp, sizeof(tmp));
            if (std::getenv("PERSONA_DEBUG_TIMELINE") && n != 0) {
                std::cerr << "dbg: pi reader read()=" << n
                          << " errno=" << (n < 0 ? std::strerror(errno) : "-") << "\n";
            }
            if (n > 0) {
                buf.append(tmp, static_cast<size_t>(n));
                // LF-only framing (pi docs/rpc.md): split on '\n' ONLY — never
                // on Unicode separators — and strip a trailing '\r' so
                // \r\n-terminated lines parse too.
                size_t nl = 0;
                while ((nl = buf.find('\n')) != std::string::npos) {
                    std::string line = buf.substr(0, nl);
                    buf.erase(0, nl + 1);
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    handle_line(line);
                }
                continue;
            }
            if (n == 0) {
                break;  // EOF: the child closed stdout (normal exit or crash)
            }
            if (errno == EINTR) {
                continue;
            }
            fire_error(std::string("pi stdout read error: ") + std::strerror(errno));
            break;
        }
        if (std::getenv("PERSONA_DEBUG_TIMELINE")) {
        std::cerr << "dbg: pi reader EOF (shutting_down_=" << shutting_down_.load()
                  << ") read_fd_=" << read_fd_ << "\n";
    }
    eof_ = true;
        if (!shutting_down_.load()) {
            // The child died on its own (not via shutdown()): surface it so
            // the daemon emits agent.error and keeps running.
            fire_error("pi exited (stdout closed)");
        }
    });

    return true;
}

bool PiAgent::running() const {
    return started_ && !shutting_down_.load() && !eof_.load();
}

void PiAgent::submit_prompt(int seq, const std::string& text) {
    if (!running()) {
        return;  // drop silently
    }
    const nlohmann::json cmd = {{"type", "prompt"},
                                {"message", text},
                                {"streamingBehavior", "steer"}};
    const std::string line = cmd.dump() + "\n";
    // Single writer thread; loop for partial writes. The daemon ignores
    // SIGPIPE, so a write to a dead pipe surfaces as EPIPE here (-> on_error;
    // the reader's EOF confirms).
    size_t off = 0;
    while (off < line.size()) {
        const ssize_t n = ::write(write_fd_, line.data() + off, line.size() - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fire_error(std::string("write to pi stdin failed: ") + std::strerror(errno));
            return;
        }
        off += static_cast<size_t>(n);
    }
    if (std::getenv("PERSONA_DEBUG_TIMELINE")) {
        std::cerr << "dbg: pi prompt seq=" << seq << " text='" << text << "'\n";
    }
}

void PiAgent::handle_line(const std::string& line) {
    nlohmann::json obj;
    try {
        obj = nlohmann::json::parse(line);
    } catch (const std::exception& ex) {
        // Garbage on pi's stdout (a stub bug, or pi misbehaving): skip the
        // line, never crash the daemon.
        if (std::getenv("PERSONA_DEBUG_TIMELINE")) {
            std::cerr << "dbg: pi_rpc: non-JSON line ignored: " << ex.what()
                      << "  line=" << line << "\n";
        }
        return;
    }
    if (!obj.is_object()) {
        return;
    }
    const std::string type = obj.value("type", std::string());

    if (type == "message_update") {
        const nlohmann::json ev = obj.value("assistantMessageEvent", nlohmann::json());
        const std::string et = ev.value("type", std::string());
        if (et == "text_delta") {
            std::string delta = ev.value("delta", std::string());
            // Cap the informational accumulation (v1: only read for debug
            // logging): a long-running daemon must not grow memory
            // unboundedly with total reply volume (T9 review P2).
            if (reply_buffer_.size() < kReplyBufferCap) {
                reply_buffer_ += delta;
            }
            if (ev_.on_reply_delta) {
                ev_.on_reply_delta(std::move(delta));
            }
        }
        // text_start/text_end, thinking_*, toolcall_*: ignored — the daemon
        // only speaks the final text (message_end is authoritative).
        return;
    }

    if (type == "message_end") {
        // Settle the per-reply accumulation (informational only).
        reply_buffer_.clear();
        const std::string text =
            extract_message_text(obj.value("message", nlohmann::json()));
        if (ev_.on_reply_complete) {
            // Fire even for empty text: a completed turn must always settle
            // the daemon's outstanding-reply accounting (T9 review P1-2).
            ev_.on_reply_complete(std::move(text));
        }
        return;
    }

    if (type == "response") {
        const bool success = obj.value("success", false);
        if (!success) {
            // A rejected prompt is a COMPLETED turn with no message_end to
            // follow — route through on_prompt_rejected so the daemon settles
            // its accounting (and still surfaces agent.error).
            if (ev_.on_prompt_rejected) {
                ev_.on_prompt_rejected("prompt rejected: " + obj.value("error", std::string()));
            } else {
                fire_error("prompt rejected: " + obj.value("error", std::string()));
            }
        }
        return;
    }

    // agent_start/end, turn_*, bash_execution_update, tool_execution_*,
    // queue_update, compaction_*, extension_error, ...: not relevant to the
    // v1 reply path — ignored silently.
}

void PiAgent::shutdown() {
    if (!started_) {
        return;
    }
    if (shutting_down_.exchange(true)) {
        return;  // idempotent
    }
    // SIGTERM the child (the accepted RPC shutdown), escalating to SIGKILL
    // after a short grace so a child that ignores SIGTERM never hangs daemon
    // shutdown. The reader thread hits EOF as soon as the child — the pipe's
    // only writer — dies.
    if (pid_ > 0) {
        ::kill(pid_, SIGTERM);
        int status = 0;
        for (int i = 0; i < 50; ++i) {
            const pid_t r = ::waitpid(pid_, &status, WNOHANG);
            if (r == pid_) {
                pid_ = -1;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
            pid_ = -1;
        }
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    if (read_fd_ >= 0) {
        ::close(read_fd_);
        read_fd_ = -1;
    }
    if (write_fd_ >= 0) {
        ::close(write_fd_);
        write_fd_ = -1;
    }
    started_ = false;
}

void PiAgent::fire_error(const std::string& err) {
    if (ev_.on_error) {
        ev_.on_error(err);
    }
}

}  // namespace persona
