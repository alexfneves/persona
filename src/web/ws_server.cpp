#include "web/ws_server.h"

#include "web/page.h"

#include <websocketpp/common/connection_hdl.hpp>
#include <websocketpp/frame.hpp>
#include <websocketpp/http/constants.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace persona {

namespace {

// Out-queue cap: one item per TTS reply / event. 8 bounds worst-case buffered
// memory (~8 full replies) while leaving room for a burst of status events
// after a slow synthesis. Overflow drops the item (same philosophy as the PA
// playback queue's drop-on-overflow) — the pipeline never blocks on WS.
constexpr size_t kMaxOutItems = 8;

// i16 -> f32 (mirror of read_stdin_s16le_f32, listen.cpp).
inline float s16_to_f32(int16_t s) { return static_cast<float>(s) / 32768.0f; }

}  // namespace

WebServer::WebServer(std::string addr, int port)
    : addr_(std::move(addr)), port_(port) {}

WebServer::~WebServer() { stop(); }

bool WebServer::start() {
    try {
        server_.init_asio();
    } catch (const std::exception& ex) {
        std::cerr << "daemon: web: init_asio failed: " << ex.what() << "\n";
        return false;
    }
    started_ = true;  // stop() is now safe even if listen/start_accept fail

    // Silence access logs: websocketpp's access logger writes to STDOUT by
    // default — daemon stdout must stay pure NDJSON (ISC-A-3). Error logs
    // (stderr) stay on so wire-level failures are diagnosable.
    server_.clear_access_channels(websocketpp::log::alevel::all);
    server_.set_reuse_addr(true);

    setup_handlers();

    websocketpp::lib::error_code ec;
    server_.listen(addr_, std::to_string(port_), ec);
    if (ec) {
        std::cerr << "daemon: web: cannot listen on " << addr_ << ":" << port_
                  << ": " << ec.message() << "\n";
        return false;
    }
    server_.start_accept(ec);
    if (ec) {
        std::cerr << "daemon: web: start_accept failed: " << ec.message() << "\n";
        return false;
    }

    // Resolve the ACTUAL bound port (--web-port 0 -> ephemeral; the test
    // suite parses this stderr line).
    {
        websocketpp::lib::asio::error_code lec;
        const auto local = server_.get_local_endpoint(lec);
        actual_port_ = lec ? port_ : static_cast<int>(local.port());
    }

    thread_ = std::thread([this] { server_.run(); });

    std::cerr << "daemon: web server listening on " << addr_ << ":" << actual_port_
              << "\n";
    return true;
}

void WebServer::stop() {
    if (!started_) {
        return;
    }
    stop_.store(true, std::memory_order_release);
    // Close the active connection (going_away) so the client sees a clean
    // close frame; then stop accepting and stop the io_context so run()
    // returns. io_context::stop is thread-safe from here.
    if (active_.exchange(false, std::memory_order_acq_rel)) {
        websocketpp::lib::error_code ec;
        server_.close(hdl_, websocketpp::close::status::going_away,
                      "daemon shutdown", ec);
        if (ec) {
            std::cerr << "daemon: web: close during shutdown: " << ec.message()
                      << "\n";
        }
    }
    // stop_listening throws on a second call (state != LISTENING) — use the
    // ec overload so stop() is idempotent (the daemon may call it from the
    // shutdown path and again from the destructor).
    {
        websocketpp::lib::error_code sec;
        server_.stop_listening(sec);
    }
    server_.get_io_context().stop();

    if (thread_.joinable()) {
        // run() returns once the io_context is stopped; our handlers never
        // block, so the join cannot hang. (If it somehow did, the process
        // _Exits right after anyway.) A second stop() finds the thread
        // already joined and does nothing.
        thread_.join();
    }
}

void WebServer::setup_handlers() {
    using websocketpp::lib::placeholders::_1;
    using websocketpp::lib::bind;

    // One-connection rule (ISC-2): reject a second upgrade at the HTTP level
    // (validate runs before any WS state is created); the first stays up.
    server_.set_validate_handler(bind(&WebServer::validate_cb, this, _1));

    server_.set_open_handler(bind(&WebServer::open_cb, this, _1));
    server_.set_close_handler(bind(&WebServer::close_cb, this, _1));

    server_.set_message_handler(
        [this](websocketpp::connection_hdl hdl, Server::message_ptr msg) {
            if (msg->get_opcode() == websocketpp::frame::opcode::BINARY) {
                const std::string& p = msg->get_payload();
                // Mono PCM16 LE @ 16 kHz -> f32, fed to the same VAD/ASR
                // 512-chunk path as the mic (ISC-3). Frame size is arbitrary
                // (the pipeline accumulates to full 512-sample chunks).
                for (size_t i = 0; i + 1 < p.size(); i += 2) {
                    const int16_t s = static_cast<int16_t>(
                        static_cast<uint16_t>(static_cast<uint8_t>(p[i])) |
                        (static_cast<uint16_t>(static_cast<uint8_t>(p[i + 1]))
                         << 8));
                    in_ring_.push(s16_to_f32(s));
                }
            } else {
                // Text control frames are optional in v1 — log and ignore.
                std::cerr << "daemon: web: ignoring text frame '"
                          << msg->get_payload() << "'\n";
            }
        });

    // Serves the embedded page at GET /; anything else is 404.
    server_.set_http_handler(
        [this](websocketpp::connection_hdl hdl) {
            Server::connection_ptr con = server_.get_con_from_hdl(hdl);
            if (con->get_request().get_method() != "GET" ||
                con->get_request().get_uri() != "/") {
                con->set_status(websocketpp::http::status_code::not_found);
                con->set_body("404 Not Found");
                return;
            }
            con->set_status(websocketpp::http::status_code::ok);
            con->append_header("Content-Type", "text/html; charset=utf-8");
            con->set_body(kPageHtml);
        });
}

bool WebServer::validate_cb(websocketpp::connection_hdl) {
    if (active_.load(std::memory_order_acquire)) {
        // Second connection: reject (websocketpp answers 403). The first
        // connection stays up untouched.
        std::cerr << "daemon: web: rejecting second connection — one client "
                     "already active\n";
        return false;
    }
    return true;
}

void WebServer::open_cb(websocketpp::connection_hdl hdl) {
    hdl_ = hdl;
    active_.store(true, std::memory_order_release);
    if (hello_.is_object()) {
        websocketpp::lib::error_code ec;
        server_.send(hdl, hello_.dump(), websocketpp::frame::opcode::text, ec);
        if (ec) {
            std::cerr << "daemon: web: hello send failed: " << ec.message() << "\n";
        }
    }
}

void WebServer::close_cb(websocketpp::connection_hdl) {
    active_.store(false, std::memory_order_release);
    std::cerr << "daemon: web: client disconnected — listener stays up "
                 "awaiting reconnect\n";
}

bool WebServer::enqueue_audio(AudioBufferPcm buf) {
    if (!active_.load(std::memory_order_acquire)) {
        return false;  // no client — nothing to deliver
    }
    OutItem item;
    item.kind = OutItem::Kind::Audio;
    item.audio = std::move(buf);
    item.gen = gen_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(out_mx_);
        if (out_q_.size() >= kMaxOutItems) {
            std::cerr << "daemon: web: out queue full — audio dropped\n";
            return false;
        }
        out_q_.push_back(std::move(item));
    }
    post_drain();
    return true;
}

void WebServer::flush_audio() {
    // Bump the generation FIRST: every audio item queued (or queued later by
    // an in-flight synthesis) under the previous gen is stale and skipped at
    // drain.
    gen_.fetch_add(1, std::memory_order_relaxed);
    OutItem item;
    item.kind = OutItem::Kind::Control;
    item.json = {{"type", "audio.flush"}};
    enqueue_control(std::move(item));
}

void WebServer::send_event(nlohmann::json j) {
    OutItem item;
    item.kind = OutItem::Kind::Control;
    item.json = std::move(j);
    enqueue_control(std::move(item));
}

bool WebServer::enqueue_control(OutItem item) {
    if (!active_.load(std::memory_order_acquire)) {
        return false;  // no client
    }
    {
        std::lock_guard<std::mutex> lk(out_mx_);
        if (out_q_.size() >= kMaxOutItems) {
            std::cerr << "daemon: web: out queue full — event dropped\n";
            return false;
        }
        out_q_.push_back(std::move(item));
    }
    post_drain();
    return true;
}

void WebServer::post_drain() {
    // Coalesced drain: at most one drain job in flight; the drain re-posts
    // if a producer raced in during its final re-check.
    if (drain_pending_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    // Boost.Asio free-function post (this nixpkgs' io_context has no member
    // post with a single handler arg — the TS-executor overloads need an
    // allocator). Thread-safe: the drain runs on the asio thread.
    boost::asio::post(server_.get_io_context(), [this] { drain(); });
}

void WebServer::drain() {
    // Asio thread only. Snapshot the current gen once: audio enqueued under a
    // newer gen (a flush landed mid-drain) is skipped HERE and picked up by
    // the re-posted drain (its gen matches there) — nothing is lost.
    const uint64_t cur_gen = gen_.load(std::memory_order_relaxed);
    for (;;) {
        OutItem item;
        {
            std::lock_guard<std::mutex> lk(out_mx_);
            if (out_q_.empty()) {
                break;
            }
            item = std::move(out_q_.front());
            out_q_.pop_front();
        }
        websocketpp::lib::error_code ec;
        if (item.kind == OutItem::Kind::Audio) {
            if (item.gen != cur_gen) {
                continue;  // stale — flushed by a barge-in
            }
            // f32 -> i16 clamped, LE bytes (mirror of the capture side).
            const auto& samples = item.audio.samples;
            std::string bytes;
            bytes.resize(samples.size() * 2);
            for (size_t i = 0; i < samples.size(); ++i) {
                float f = samples[i] * 32768.0f;
                f = std::max(-32768.0f, std::min(32767.0f, f));
                const int16_t s = static_cast<int16_t>(f);
                bytes[2 * i] = static_cast<char>(static_cast<uint8_t>(s & 0xFF));
                bytes[2 * i + 1] = static_cast<char>(
                    static_cast<uint8_t>((s >> 8) & 0xFF));
            }
            server_.send(hdl_, bytes.data(), bytes.size(),
                         websocketpp::frame::opcode::binary, ec);
            if (ec) {
                std::cerr << "daemon: web: audio send failed: " << ec.message()
                          << "\n";
            }
        } else {
            server_.send(hdl_, item.json.dump(), websocketpp::frame::opcode::text,
                         ec);
            if (ec) {
                std::cerr << "daemon: web: event send failed: " << ec.message()
                          << "\n";
            }
        }
    }
    drain_pending_.store(false, std::memory_order_release);
    // Re-check under the lock: a producer may have pushed after our last pop
    // but before the store (its exchange saw a drain "in flight").
    {
        std::lock_guard<std::mutex> lk(out_mx_);
        if (!out_q_.empty() && !drain_pending_.exchange(true)) {
            boost::asio::post(server_.get_io_context(), [this] { drain(); });
        }
    }
}

}  // namespace persona
