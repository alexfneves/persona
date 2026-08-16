#pragma once

// WebSocket audio in/out server for web mode (T7, F1). The daemon serves an
// embedded HTML page at GET / and upgrades WS connections at the same path:
// the browser is both the mic (binary mono PCM16 @ 16 kHz -> VAD/ASR) and the
// speaker (TTS streams back as binary mono PCM16 @ 24 kHz plus JSON control
// events). Protocol (v1):
//
//   C->S binary  mono PCM16 LE @ 16 kHz, any frame size   mic audio
//   C->S text    {"type":"hello"} / {"type":"bye"}        control (optional)
//   S->C binary  mono PCM16 LE @ 24 kHz                   TTS audio
//   S->C text    {"type":"hello", ...}                    on connect
//   S->C text    {"type":"audio.flush"}                   barge-in: stop playback
//   S->C text    mirrored NDJSON events                   status
//   S->C text    {"type":"error","error":...}             errors
//   S->C text    {"type":"bye","reason":...}              server closing
//
// Threading (ISC-A-1/2): exactly ONE asio thread owns every socket read and
// write. The pipeline thread feeds the in-ring (the asio thread's message
// handler pushes i16->f32) and the out-queue (mutex + deque; the asio thread
// drains it via a coalesced io_context.post job). The pipeline never blocks
// on WS I/O: enqueue_audio/send_event drop on overflow (same philosophy as
// the PA playback queue) and the drain never sends synchronously with the
// pipeline thread. One active connection at a time (a second upgrade is
// rejected in the validate handler, HTTP-level, before any WS state).

#include "audio/playback.h"  // AudioBufferPcm
#include "audio/ringbuf.h"   // RingBuffer<float>

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace persona {

class WebServer {
public:
    // addr/port: the listen address (--web-addr, default 127.0.0.1) and port
    // (--web-port, default 8765; 0 = ephemeral — the actual port is logged to
    // stderr by start()).
    WebServer(std::string addr, int port);
    ~WebServer();

    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    // Binds the listener, arms acceptance, spawns the single asio thread.
    // Logs "daemon: web server listening on <addr>:<port>" (the ACTUAL port
    // when --web-port 0) to stderr. Returns false on bind/listen failure
    // (the daemon then exits with an error — a busy port is a startup error,
    // not a degraded mode).
    bool start();

    // Closes the active connection (going_away), stops listening, stops the
    // io_context and joins the asio thread. Safe to call twice; never hangs —
    // if the thread is wedged the join falls back to detach (mirrors the
    // stdin-thread pattern; the process _Exits right after anyway).
    void stop();

    // ---- SampleSource body (pipeline thread) ----
    // Pops up to n f32 samples fed by the WS client. Never blocks; 0 means
    // "no audio right now". The in-ring is SPSC: the asio thread pushes
    // (single producer), this pulls (single consumer, the pipeline).
    size_t pull(float* out, size_t n) { return in_ring_.pop_up_to(out, n); }

    // A std::function compatible with the daemon's SampleSource (third
    // implementation, after mic and fixture).
    std::function<size_t(float* out, size_t n)> make_source() {
        return [this](float* out, size_t n) { return pull(out, n); };
    }

    // ---- out-queue producers (pipeline thread) ----
    // Queues one TTS reply buffer for the client (encoded to PCM16 LE on the
    // asio thread). Drops (logs + returns false) when no client is connected
    // or the out-queue is full — the pipeline never blocks on WS. An item
    // queued before a flush_audio() is dropped at drain time (stale gen).
    bool enqueue_audio(AudioBufferPcm buf);

    // Barge-in (F3): bumps the generation so queued audio is skipped at drain
    // and enqueues a {"type":"audio.flush"} control frame telling the page to
    // stop its current AudioBufferSourceNode and clear its queue.
    void flush_audio();

    // Mirrors one NDJSON event to the client as a text frame (the daemon's
    // emit_all wrapper). Drops when no client is connected or full.
    void send_event(nlohmann::json j);

    // The hello payload sent to a client on connect ({"type":"hello", ...});
    // set once before start() by the daemon (carries vad/asr/tts info per the
    // protocol table). No lock: written before the asio thread exists, read
    // on the asio thread's open handler.
    void set_hello(nlohmann::json hello) { hello_ = std::move(hello); }

    // True while exactly one client is connected (daemon disconnect-detection
    // hook: a false transition mid-utterance force-finalizes, per T8).
    bool connected() const { return active_.load(std::memory_order_acquire); }

    int actual_port() const { return actual_port_; }

private:
    using Server = websocketpp::server<websocketpp::config::asio>;

    struct OutItem {
        enum class Kind { Audio, Control };
        Kind kind = Kind::Control;
        AudioBufferPcm audio;      // Kind::Audio
        nlohmann::json json;       // Kind::Control
        uint64_t gen = 0;          // Kind::Audio: generation at enqueue
    };

    // Produces the Hello + registers the validate/open/close/message/http
    // handlers. Called once, before the asio thread exists.
    void setup_handlers();
    // websocketpp handler bodies (asio thread): validate rejects a second
    // upgrade; open/close toggle active_ + send the hello / log;
    bool validate_cb(websocketpp::connection_hdl hdl);
    void open_cb(websocketpp::connection_hdl hdl);
    void close_cb(websocketpp::connection_hdl hdl);

    // Producer side of the out-queue: pushes + coalesced drain post.
    bool enqueue_control(OutItem item);
    void post_drain();

    // Asio thread only: pops the out-queue, encodes/sends frames; skips stale
    // audio (gen mismatch). Sets drain_pending_ false last, re-posting if the
    // producer raced in (standard double-check).
    void drain();

    std::string addr_;
    int port_ = 0;
    int actual_port_ = 0;

    Server server_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    bool started_ = false;  // start() ran init_asio; stop() is safe

    // SPSC in-ring: producer = asio thread (message handler), consumer =
    // pipeline thread (pull). 65536 floats = 4 s @ 16 kHz (matches capture).
    RingBuffer in_ring_{65536};

    // Out-queue: producer = pipeline thread, consumer = asio thread (drain).
    // Mutex+deque — items are rare (one per reply/event) and the critical
    // sections are microseconds.
    std::mutex out_mx_;
    std::deque<OutItem> out_q_;
    std::atomic<bool> drain_pending_{false};

    // Audio generation: bumped by flush_audio(); audio items carry the gen at
    // enqueue and are skipped when it differs from the drain's current gen.
    std::atomic<uint64_t> gen_{0};

    // One-connection rule + disconnect detection.
    std::atomic<bool> active_{false};
    websocketpp::connection_hdl hdl_;

    nlohmann::json hello_;  // sent on open (set before start())
};

}  // namespace persona
