#pragma once

// RPC client for the pi coding agent (plan Decision 8, T12): spawns
// `pi --mode rpc` as a child process with its OWN stdin/stdout pipes and
// speaks the JSONL protocol from pi's docs/rpc.md (verified against that doc:
// LF-only framing with optional trailing \r; split on '\n' only, never on
// Unicode separators). The daemon's speech.final text is submitted as a
// `prompt` command with streamingBehavior "steer" (queues while pi is
// mid-turn); pi's replies arrive as message_update text_delta events and the
// authoritative turn_end message.
//
// Threading contract:
//   * start() spawns the child AND a READER thread that consumes the child's
//     stdout, frames lines, parses nlohmann JSON, and dispatches. ALL event
//     callbacks fire on the READER thread — the daemon must marshal them onto
//     its own thread (the pipeline thread's command queue, see daemon.cpp).
//   * the child's stdin is written from ONE thread only (the daemon's pipeline
//     thread, single writer). submit_prompt never blocks the daemon loop
//     beyond the pipe write itself.
//   * This class is engine-free: it never touches audio.cpp.
//
// This is not a full RPC client — only the events the daemon's v1 reply path
// needs are handled; every other event type is ignored silently (log to stderr
// at debug level via PERSONA_DEBUG_TIMELINE).

#include <sys/types.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace persona {

class PiAgent {
public:
    struct Events {
        // A text_delta chunk of the current assistant message. v1: the daemon
        // only needs the FINAL reply text for TTS (message_end is
        // authoritative), so deltas are reserved for a future agent.partial —
        // PiAgent accumulates them internally and fires this for observability.
        std::function<void(std::string text)> on_reply_delta;
        // turn_end: the authoritative end of one assistant turn. The full
        // text of the turn's FINAL assistant message (content blocks with
        // type=="text"). The daemon speaks this. Fired EVEN for an empty text
        // (a thinking/toolCall-only turn is still a completed turn — the
        // daemon settles its outstanding-reply accounting; it decides itself
        // whether to emit agent.reply.done, only for non-empty text).
        //
        // This is the reply signal INSTEAD of message_end: pi emits one
        // message_end per assistant sub-message (a tool-using model produces
        // a thinking-only message, a toolCall message, then the text answer),
        // so firing on message_end produced DUPLICATE agent.reply.done. turn_end
        // fires exactly once per turn with the final answer.
        std::function<void(std::string full)> on_reply_complete;
        // Spawn failure, broken pipe, or child exit. The daemon emits
        // agent.error and stays up.
        std::function<void(std::string err)> on_error;
        // A prompt was REJECTED (response success:false) — no message_end will
        // follow, but the turn is complete. The daemon surfaces it as an
        // agent.error that also settles its outstanding-reply accounting
        // (distinct from on_error, which fires for child-lifecycle failures
        // not tied to a submitted prompt).
        std::function<void(std::string err)> on_prompt_rejected;
    };

    // pi_bin: the pi binary (PATH-resolved) or an explicit path (e.g.
    // PERSONA_PI_BIN -> tests/pi_stub.sh). extra_args are appended after
    // `--mode rpc` (e.g. --provider, --model).
    explicit PiAgent(Events ev, std::string pi_bin = "pi",
                     std::vector<std::string> extra_args = {});
    ~PiAgent();  // calls shutdown() if still running

    // Spawns `pi --mode rpc` (+ extra_args) with its own stdin/stdout pipes
    // and starts the reader thread. Returns false (and fires on_error) on
    // spawn failure — the caller keeps running without the agent. Idempotent.
    bool start();

    // True while the child is up and its stdout pipe is open (not shutting
    // down, no EOF/read error yet).
    bool running() const;

    int pid() const { return static_cast<int>(pid_); }

    // Writes one JSONL prompt command
    // ({"type":"prompt","message":text,"streamingBehavior":"steer"}) and
    // flushes. Drops SILENTLY when not running. Call from a single writer
    // thread (the daemon's pipeline thread). Never throws.
    void submit_prompt(int seq, const std::string& text);

    // SIGTERMs the child (the accepted RPC shutdown — pi has no documented
    // stop command; escalate to SIGKILL after a short grace), joins the reader
    // thread, reaps, and closes the pipes. Idempotent; safe to call once.
    void shutdown();

private:
    // Parses one JSON line and dispatches. Never throws (parse failures are
    // ignored/logged). Runs on the reader thread.
    void handle_line(const std::string& line);

    // Fires ev_.on_error if set. Thread-safe (only called from the reader
    // thread and from start() before the reader exists — no lock needed).
    void fire_error(const std::string& err);

    Events ev_;
    std::string pi_bin_;
    std::vector<std::string> extra_args_;

    bool started_ = false;  // written by start()/shutdown() (owner thread)
    std::atomic<bool> shutting_down_{false};
    std::atomic<bool> eof_{false};  // reader saw EOF / read error
    int read_fd_ = -1;              // child stdout (reader thread)
    int write_fd_ = -1;             // child stdin (pipeline thread writer)
    pid_t pid_ = -1;
    std::thread reader_;

    // Accumulates text_delta chunks across contentIndex into one growing
    // string per reply. v1: informational only (message_end is authoritative
    // for the reply text); kept for a future agent.partial. Reader-thread only.
    std::string reply_buffer_;
};

}  // namespace persona
