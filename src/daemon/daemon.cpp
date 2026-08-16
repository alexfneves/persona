#include "agent/pi_rpc.h"
#include "audio/capture.h"
#include "audio/playback.h"
#include "audio/wav.h"
#include "config.h"
#include "backend.h"
#include "model/registry.h"
#include "pipeline/endpointer.h"
#include "pipeline/stt.h"
#include "pipeline/tts.h"
#include "pipeline/vad.h"
#include "protocol/ndjson.h"

#include "engine/framework/core/backend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace persona {

namespace {

constexpr int kRate = 16000;   // pipeline sample rate: 16 kHz mono f32
constexpr int64_t kChunk = 512;  // silero_vad preferred chunk (samples)
constexpr auto kPartialMinInterval = std::chrono::milliseconds(250);
constexpr auto kPollInterval = std::chrono::milliseconds(10);
// How much audio before a VAD SpeechStart is fed to the ASR session as
// leading context. qwen3's 1 s streaming windows are sensitive to the exact
// audio timeline: dropping the onset chunk garbles the transcript (verified
// in T8), so each utterance's ASR session starts just before the VAD onset
// instead of exactly at it. The lead is kept SHORT (~0.13 s): a long lead
// into silence shifts window boundaries mid-word on later utterances (the
// 0.6 s pre-roll used initially garbled utterance 2 of hello_hello.wav). A
// short lead covers the onset chunk while keeping windows aligned to the
// speech onset, which is the natural boundary (T8's stdin path).
constexpr int64_t kPrerollSamples = static_cast<int64_t>(0.13 * kRate);
// Mic-idle watchdog (live-mic mode only): warn once after this long with zero
// samples received, then re-warn every kMicIdleRelog. The PortAudio input
// stream can die silently (USB suspend, device error, another app grabbing the
// mic) — the ring just stops filling and nothing else ever logs, while the
// daemon keeps running and "suddenly stops transcribing". The watchdog is the
// only place that calls into the PA stream-state API (never per-chunk).
constexpr auto kMicIdleWarn = std::chrono::seconds(5);
constexpr auto kMicIdleRelog = std::chrono::seconds(60);

// ---- signals --------------------------------------------------------------
// SIGINT/SIGTERM set this flag (async-signal-safe); the pipeline loop and the
// stdin reader both check it and trigger a graceful shutdown. SIGPIPE is
// ignored — the agent closing stdout must NOT kill the daemon (ISC-12);
// instead the failed std::cout writes surface as StdoutClosed below.
volatile std::sig_atomic_t g_terminate_signal = 0;
void on_terminate_signal(int) {
    // async-signal-safe: only set the flag; the pipeline loop performs the
    // graceful shutdown.
    g_terminate_signal = 1;
}

enum class ShutdownReason : int {
    None,
    StdinStop,
    Signal,
    StdoutClosed,
    FixtureEof,
    PipelineError,
};

const char* shutdown_reason_str(ShutdownReason r) {
    switch (r) {
    case ShutdownReason::StdinStop: return "stdin-stop";
    case ShutdownReason::Signal: return "signal";
    case ShutdownReason::StdoutClosed: return "stdout-closed";
    case ShutdownReason::FixtureEof: return "audio-fixture-eof";
    case ShutdownReason::PipelineError: return "pipeline-error";
    case ShutdownReason::None: break;
    }
    return "unknown";
}

struct DaemonFlags {
    bool fixture = false;
    std::string fixture_path;
    bool have_mic_arg = false;
    bool mic_none = false;
    int mic_index = -1;  // --mic <idx>; -1 falls back to cfg.mic_device / default
};

DaemonFlags parse_daemon_flags(const std::vector<std::string>& args) {
    DaemonFlags f;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--mic") {
            if (i + 1 >= args.size()) {
                throw std::runtime_error("daemon: --mic requires none|default|<index>");
            }
            const std::string val = args[++i];
            f.have_mic_arg = true;
            if (val == "none") {
                f.mic_none = true;
            } else if (val == "default") {
                // leave mic_index = -1 -> PortAudio default input device
            } else {
                try {
                    f.mic_index = std::stoi(val);
                } catch (const std::exception&) {
                    throw std::runtime_error("daemon: --mic expects none, default, or a device "
                                             "index, got '" + val + "'");
                }
            }
        } else if (arg == "--audio-fixture") {
            if (i + 1 >= args.size()) {
                throw std::runtime_error("daemon: --audio-fixture requires a WAV path");
            }
            f.fixture = true;
            f.fixture_path = args[++i];
        } else {
            throw std::runtime_error("daemon: unknown option '" + arg + "'");
        }
    }
    if (f.fixture && f.have_mic_arg && !f.mic_none) {
        throw std::runtime_error(
            "daemon: --audio-fixture is incompatible with --mic <index> (use --mic none)");
    }
    if (f.mic_none && !f.fixture) {
        throw std::runtime_error("daemon: --mic none requires --audio-fixture <wav>");
    }
    return f;
}

// Pull source of 16 kHz mono f32 samples: the mic ring buffer or a WAV fixture
// buffer. Never blocks; 0 means "no samples right now".
using SampleSource = std::function<size_t(float* out, size_t n)>;

SampleSource make_mic_source(Capture& cap) {
    return [&cap](float* out, size_t n) { return cap.ring().pop_up_to(out, n); };
}

SampleSource make_fixture_source(const std::vector<float>& audio, size_t& cursor) {
    return [&audio, &cursor](float* out, size_t n) {
        const size_t avail = std::min(n, audio.size() - cursor);
        if (avail > 0) {
            std::memcpy(out, audio.data() + cursor, avail * sizeof(float));
            cursor += avail;
        }
        return avail;
    };
}

// One daemon-side TTS request: what the stdin reader thread hands to the
// pipeline thread (T11). `seq` is the caller-supplied sequence echoed back in
// tts.start / tts.done / tts.error.
struct TtsRequest {
    int seq = 0;
    std::string text;
};

// One daemon-side agent (pi) event: what the PiAgent reader thread hands to
// the pipeline thread (T12). Reply carries the final assistant text (TTS on
// the pipeline thread, ISC-A-1); Error is a spawn/pipe/reply failure — the
// daemon emits agent.error and keeps running (NDJSON mode continues).
struct AgentCommand {
    enum class Kind { Reply, Error };
    Kind kind = Kind::Error;
    int seq = 0;
    std::string text;   // Kind::Reply
    std::string error;  // Kind::Error
    // Kind::Error only: true when the error is a PROMPT REJECTION (pi
    // answered {"type":"response","success":false}). A rejected prompt is
    // still a completed turn, so it must decrement outstanding_replies —
    // otherwise the fixture-EOF shutdown wait would spin the full 30 s.
    bool prompt_rejected = false;
};

}  // namespace

// persona daemon (T9): continuous mic -> NDJSON voice channel.
//
// Threads:
//   * this thread (main) is the PIPELINE thread — every audio.cpp session
//     call (VAD, ASR, TTS) happens here, serialized (ISC-A-1);
//   * a stdin-reader thread parses NDJSON commands and sets running=false on
//     stop (never touches stdout or the engine);
//   * the PortAudio callback (mic mode only) pushes samples into the ring
//     buffer and does nothing else (ISC-A-2).
//
// Pipeline loop per chunk: accumulate >= 512 samples -> feed the full chunk to
// VadSession (its callbacks fire synchronously -> Endpointer.on_vad_start/end)
// -> feed the chunk to SttSession while the endpointer is audio-active ->
// Endpointer.tick(chunk_end) for the 30 s cap -> drain Endpointer intents:
//   BeginUtterance -> emit speech.start + stt.begin_utterance()
//   EndUtterance   -> stt.end_utterance() + emit speech.final
//
// Test mode: --mic none --audio-fixture <wav> feeds the WAV through the same
// ring -> VAD -> endpointer -> ASR path (instant, deterministic); on fixture
// EOF the daemon finalizes any open utterance, emits shutdown, and exits 0.
int verb_daemon(const Config& cfg, const std::vector<std::string>& args) {
    const DaemonFlags flags = parse_daemon_flags(args);

    // Install signal handlers FIRST — before the (expensive) model load in
    // make_runtime — so a SIGINT/SIGTERM arriving during startup still gets a
    // graceful shutdown instead of the default termination (clean exit).
    // SIGPIPE is ignored: the agent closing stdout must NOT kill the daemon;
    // the failed std::cout writes surface as StdoutClosed instead (ISC-12).
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, on_terminate_signal);
    std::signal(SIGTERM, on_terminate_signal);

    Runtime rt = make_runtime(cfg);
    if (!rt.vad_model) {
        std::cerr << "daemon: silero_vad runtime not loaded\n";
        return 1;
    }
    if (!rt.asr_model) {
        std::cerr << "daemon: ASR model not loaded (" << rt.asr_family
                  << " / " << rt.asr_package << ")\n"
                  << load_failure_hint(rt, rt.asr_load_fail, rt.asr_family,
                                       cfg.asr_package, rt.asr_load_fail_detail)
                  << "\n";
        return 1;
    }

    // Compute backend for engine sessions (TTS synthesis runs on this thread
    // too). The binary is built for one backend (PERSONA_DEFAULT_BACKEND);
    // --backend overrides it, e.g. forcing CPU on a Vulkan build.
    engine::core::BackendConfig backend;
    {
        std::string berr;
        if (!persona::parse_backend(cfg.backend, backend.type, berr)) {
            std::cerr << "daemon: " << berr << "\n";
            return 1;
        }
    }
    backend.device = 0;
    backend.threads =
        std::max(1, static_cast<int>(std::thread::hardware_concurrency()));

    // Playback opens at daemon start so tts audio just enqueues (T11). The
    // open is BEST-EFFORT: with no output device (e.g. a headless
    // --mic none --audio-fixture run) the daemon keeps running — a tts
    // command then still synthesizes and reports tts.done, but the audio is
    // dropped (logged to stderr). --play-device picks the device; the stream
    // resamples every buffer to the device's fixed rate.
    Playback pb;
    bool playback_ok = false;
    try {
        pb.open(cfg.play_device, 0);  // device default rate
        pb.start();
        playback_ok = true;
        std::cerr << "daemon: playback on '" << pb.device_name() << "' (device "
                  << pb.device_index() << ", " << pb.sample_rate() << " Hz)\n";
    } catch (const std::exception& ex) {
        std::cerr << "daemon: playback unavailable (tts audio will be dropped): "
                  << ex.what() << "\n";
    }

    // tts command queue: the stdin reader thread (single producer) pushes
    // TtsRequests; the pipeline thread (single consumer) drains them. A plain
    // mutex+deque — commands are rare (one per user TTS request) and the
    // critical section is a few microseconds, so this is non-blocking in
    // practice; no lock-free machinery is warranted at this rate.
    std::mutex tts_mutex;
    std::deque<TtsRequest> tts_commands;

    // agent (pi) command queue: the PiAgent READER thread (single producer)
    // pushes AgentCommands; the pipeline thread (single consumer) drains them.
    // Same mutex+deque rationale as tts_commands. Reply text is marshaled
    // here (not synthesized on the reader thread) so ALL engine calls stay on
    // the pipeline thread (ISC-A-1).
    std::mutex agent_mutex;
    std::deque<AgentCommand> agent_commands;
    // Reply sequence tracking: the pipeline thread stores the seq of the most
    // recent prompt before submitting it (PiAgent's Events carry no seq); the
    // reader thread reads it back when message_end arrives. Prompts are
    // strictly sequential (utterances are), so this maps a reply to its
    // utterance. The atomic release/acquire pair makes the store visible to
    // the reader long before any pipe round-trip (50 ms+ in practice).
    std::atomic<int> pending_reply_seq{0};
    // Prompts submitted minus replies drained (pipeline-thread only; the
    // wait-for-replies loop at shutdown reads it on the same thread).
    int outstanding_replies = 0;

    // --agent pi: spawn the RPC child. The callbacks below fire on the
    // PiAgent READER thread and only enqueue marshaled work; they never touch
    // the engine, stdout, or the playback queue. Spawn failure (e.g. pi not
    // installed) fires on_error -> agent.error and the daemon continues as
    // NDJSON-only.
    std::unique_ptr<PiAgent> pi;
    if (cfg.agent == "pi") {
        PiAgent::Events ev;
        ev.on_reply_delta = [](std::string delta) {
            // v1: deltas are informational — message_end is authoritative for
            // the reply text (a future agent.partial will use these).
            if (getenv("PERSONA_DEBUG_TIMELINE")) {
                std::cerr << "dbg: pi delta '" << delta << "'\n";
            }
        };
        ev.on_reply_complete = [&](std::string full) {
            AgentCommand cmd;
            cmd.kind = AgentCommand::Kind::Reply;
            cmd.seq = pending_reply_seq.load();
            cmd.text = std::move(full);
            std::lock_guard<std::mutex> lock(agent_mutex);
            agent_commands.push_back(std::move(cmd));
        };
        ev.on_error = [&](std::string err) {
            AgentCommand cmd;
            cmd.kind = AgentCommand::Kind::Error;
            cmd.error = std::move(err);
            std::lock_guard<std::mutex> lock(agent_mutex);
            agent_commands.push_back(std::move(cmd));
        };
        // A prompt REJECTION (response success:false) is a completed turn even
        // though no message_end will arrive — surface it as an agent.error that
        // also settles outstanding_replies (P1-2; on_error does not, because
        // spawn/pipe/child-death errors are not tied to a submitted prompt).
        ev.on_prompt_rejected = [&](std::string err) {
            AgentCommand cmd;
            cmd.kind = AgentCommand::Kind::Error;
            cmd.error = std::move(err);
            cmd.prompt_rejected = true;
            std::lock_guard<std::mutex> lock(agent_mutex);
            agent_commands.push_back(std::move(cmd));
        };
        // PERSONA_PI_BIN overrides the pi binary path (decided over a --pi-bin
        // flag to keep the flag surface minimal; documented in todos).
        const char* env_bin = getenv("PERSONA_PI_BIN");
        const std::string pi_bin = env_bin ? env_bin : "pi";
        pi = std::make_unique<PiAgent>(std::move(ev), pi_bin, cfg.pi_args);
        if (pi->start()) {
            std::cerr << "daemon: agent pi spawned (pid " << pi->pid() << ")"
                      << (env_bin ? std::string(" via PERSONA_PI_BIN=" + pi_bin) : std::string())
                      << "\n";
        } else {
            // start() already fired on_error -> agent.error will be drained and
            // emitted; the daemon keeps working as NDJSON-only.
            std::cerr << "daemon: --agent pi failed to spawn '" << pi_bin
                      << "' — continuing without the agent (set PERSONA_PI_BIN to a "
                         "pi binary or stub)\n";
        }
    }

    // Audio source: fixture WAV (no PortAudio) or the mic.
    std::unique_ptr<Capture> cap;
    std::vector<float> fixture;
    size_t fixture_cursor = 0;
    SampleSource source;
    if (flags.fixture) {
        fixture = read_wav_f32(flags.fixture_path);
        if (fixture.empty()) {
            std::cerr << "daemon: audio fixture produced no samples: "
                      << flags.fixture_path << "\n";
            return 1;
        }
        source = make_fixture_source(fixture, fixture_cursor);
    } else {
        cap = std::make_unique<Capture>();
        if (flags.mic_index >= 0) {
            cap->open_mic(flags.mic_index, kRate);
        } else if (cfg.mic_device >= 0) {
            cap->open_mic(cfg.mic_device, kRate);
        } else {
            cap->open_default_mic(kRate);
        }
        cap->start();
        source = make_mic_source(*cap);
    }

    // The endpointing core. Threshold stays at silero's default (0.5); the
    // endpoint latency is cfg.vad_min_silence_ms (--vad-min-silence-ms, 800):
    // sustained silence at or above that ends the utterance (silero emits
    // SpeechEnd after min_silence_duration_ms of sub-neg-threshold audio). The
    // 30 s cap is cfg.utt_cap_s (--utt-cap-s).
    Endpointer ep(static_cast<int64_t>(cfg.utt_cap_s) * kRate);

    // Shared pipeline state captured by the session event lambdas. All of it
    // is touched only from this thread except running/shutdown_reason
    // (atomics written by the stdin reader thread).
    std::atomic<bool> running{true};
    std::atomic<int> shutdown_reason{static_cast<int>(ShutdownReason::None)};
    int64_t chunk_start = 0;  // start sample of the chunk being fed (VAD cb)
    std::string final_text;
    bool final_received = false;
    // Partial throttle state (≤1 per 250 ms per utterance). The first partial
    // always goes out; the gate then enforces the interval. (Using
    // time_point::min() as "never" would overflow the now - min subtraction.)
    bool first_partial_pending = true;
    auto last_partial_at = std::chrono::steady_clock::time_point{};

    // Pre-roll ring: the last kPrerollSamples fed to the VAD, with the
    // absolute sample position of the first buffered sample. Consumed (and
    // cleared) when an utterance begins so the ASR sees the same leading
    // silence the VAD saw before the SpeechStart.
    std::deque<float> preroll;
    int64_t preroll_base = 0;
    const auto push_preroll = [&](const std::vector<float>& chunk, int64_t start_sample) {
        if (preroll.empty()) {
            preroll_base = start_sample;
        }
        preroll.insert(preroll.end(), chunk.begin(), chunk.end());
        if (static_cast<int64_t>(preroll.size()) > kPrerollSamples) {
            const size_t drop =
                static_cast<size_t>(preroll.size() - kPrerollSamples);
            preroll.erase(preroll.begin(), preroll.begin() + static_cast<long>(drop));
            preroll_base += static_cast<int64_t>(drop);
        }
    };

    SttSession::Events stt_ev;
    stt_ev.on_partial = [&](std::string text) {
        // Throttle: at most one speech.partial per 250 ms per utterance.
        const auto now = std::chrono::steady_clock::now();
        if (getenv("PERSONA_DEBUG_TIMELINE")) {
            std::cerr << "dbg: partial seq=" << ep.seq() << " text='" << text << "'\n";
        }
        if (first_partial_pending || now - last_partial_at >= kPartialMinInterval) {
            first_partial_pending = false;
            last_partial_at = now;
            if (!protocol::emit(protocol::speech_partial(ep.seq(), text))) {
                running = false;
                shutdown_reason = static_cast<int>(ShutdownReason::StdoutClosed);
            }
        }
    };
    stt_ev.on_final = [&](std::string text) {
        final_text = std::move(text);
        final_received = true;
    };
    stt_ev.on_error = [&](std::string err) {
        if (!protocol::emit(protocol::speech_error(ep.seq(), err))) {
            running = false;
            shutdown_reason = static_cast<int>(ShutdownReason::StdoutClosed);
        }
    };
    SttSession stt(*rt.asr_model, stt_ev, cfg.asr_language);
    // Pre-create the first ASR session while the pipeline is idle: session
    // creation takes ~0.7 s and would otherwise block the loop at the first
    // VAD onset, dropping the start of the utterance ("tell me a joke" -> "a
    // joke"). begin_utterance() then only pays start_stream (~ms).
    stt.prepare(backend);

    VadSession::Events vad_ev;
    // The VAD fires these synchronously inside vad.feed(); `chunk_start` is
    // the absolute sample of the chunk being fed (within one 512-sample chunk
    // of the engine's exact event sample, see T7).
    vad_ev.on_speech_start = [&] {
        if (getenv("PERSONA_DEBUG_TIMELINE")) {
            std::cerr << "dbg: vad start at " << chunk_start << "\n";
        }
        ep.on_vad_start(chunk_start);
    };
    vad_ev.on_speech_end = [&] {
        if (getenv("PERSONA_DEBUG_TIMELINE")) {
            std::cerr << "dbg: vad end at " << chunk_start << " (state="
                      << static_cast<int>(ep.state()) << ")\n";
        }
        ep.on_vad_end(chunk_start);
    };

    std::unordered_map<std::string, std::string> vad_opts;
    // silero reads ALL tuning from SessionOptions.options (keys verified in
    // src/models/silero_vad/session.cpp: threshold, min_speech_duration_ms,
    // min_silence_duration_ms). T13 exposes all three as flags.
    {
        std::ostringstream thr;
        thr << cfg.vad_threshold;
        vad_opts["threshold"] = thr.str();
    }
    vad_opts["min_speech_duration_ms"] = std::to_string(cfg.vad_min_speech_ms);
    vad_opts["min_silence_duration_ms"] = std::to_string(cfg.vad_min_silence_ms);
    VadSession vad(*rt.vad_model, vad_ev);
    vad.start(vad_opts, backend);  // throws on setup failure -> caught by main

    // Ready line: the first thing on stdout. Pure NDJSON from here on — every
    // log line goes to stderr. T13: ready echoes the RESOLVED family + package
    // ids (e.g. "asr_package":"qwen3_asr_1_7b_q8_0") and the compiled-in
    // backend. The TTS family/package are echoed only when the model is loaded
    // (T10's make_runtime eager soft-fail); "tts":"none" tells the agent the
    // tts command will answer tts.error. With --agent pi the ready line also
    // carries "agent":"pi".
    const char* tts_family = rt.tts_model ? rt.tts_family.c_str() : "none";
    const std::string tts_package = rt.tts_model ? rt.tts_package : "";
    const std::string agent_name = cfg.agent == "pi" ? "pi" : "";
    if (!protocol::emit(protocol::ready(rt.asr_family, tts_family, "silero_vad",
                                        kRate, rt.asr_package, tts_package,
                                        persona::default_backend(), agent_name))) {
        std::cerr << "daemon: stdout closed at startup\n";
        return 0;
    }
    const auto daemon_started = std::chrono::steady_clock::now();

    // stdin reader thread: parses NDJSON commands. On {"type":"stop"} it sets
    // running=false and returns (ISC-7); malformed lines are logged to stderr
    // and skipped (ISC-11); tts commands are queued for the pipeline thread
    // (T11). Never writes to stdout.
    std::thread stdin_thread([&] {
        std::string line;
        while (running.load() && std::getline(std::cin, line)) {
            if (line.empty()) {
                continue;  // bare Enter — not a protocol message, not an error
            }
            const protocol::Command cmd = protocol::parse_command(line);
            switch (cmd.kind) {
            case protocol::CommandKind::Stop:
                shutdown_reason = static_cast<int>(ShutdownReason::StdinStop);
                running = false;
                return;
            case protocol::CommandKind::Tts: {
                TtsRequest req;
                req.seq = cmd.seq;
                req.text = std::move(cmd.text);
                std::lock_guard<std::mutex> lock(tts_mutex);
                tts_commands.push_back(std::move(req));
                break;
            }
            case protocol::CommandKind::Unknown:
                if (!cmd.error.empty()) {
                    std::cerr << "daemon: ignoring malformed stdin line: " << cmd.error
                              << "\n  line: " << line << "\n";
                }
                break;
            }
        }
        if (running.load()) {
            std::cerr << "daemon: stdin closed; continuing (send {\"type\":\"stop\"} on stdin, "
                         "a signal, or close stdout to exit)\n";
        }
    });

    // Drains stdin-queued tts commands on the pipeline thread (T11). All TTS
    // session calls happen here — serialized with the VAD/ASR calls, never
    // concurrently (ISC-A-1). SERIALIZATION NOTE: a long TTS synthesis delays
    // speech.partial/final processing for its duration (the TTS run is inline
    // with the audio loop). Acceptable for v1 — TTS is quick and there is no
    // queue-priority work yet; the plan defers that to v2. Never throws (every
    // engine call is inside TtsSession::run's non-throwing contract, and the
    // rest cannot throw) — the pipeline thread cannot be crashed by a tts
    // command. Returns false when stdout is closed (graceful shutdown).
    const auto drain_tts_commands = [&]() -> bool {
        for (;;) {
            TtsRequest req;
            {
                std::lock_guard<std::mutex> lock(tts_mutex);
                if (tts_commands.empty()) {
                    return true;
                }
                req = std::move(tts_commands.front());
                tts_commands.pop_front();
            }
            if (getenv("PERSONA_DEBUG_TIMELINE")) {
                std::cerr << "dbg: tts req seq=" << req.seq << " text='" << req.text << "'\n";
            }
            // (a) tts.start, then the synthesis itself.
            if (!protocol::emit(protocol::tts_start(req.seq))) {
                running = false;
                shutdown_reason = static_cast<int>(ShutdownReason::StdoutClosed);
                return false;
            }
            if (!rt.tts_model) {
                // T10's make_runtime eager soft-fail left tts_model null (the
                // model is not installed). Keep the daemon up — the speech
                // path is unaffected; surface the install hint and move on.
                if (!protocol::emit(protocol::tts_error(
                        req.seq, "TTS model not loaded — install it with:  " +
                                     install_hint(rt.tts_family, cfg.tts_package)))) {
                    running = false;
                    shutdown_reason = static_cast<int>(ShutdownReason::StdoutClosed);
                    return false;
                }
                continue;
            }
            // (b) Synthesize (empty text is a TtsSession error -> tts.error).
            const TtsSession::Result res = TtsSession::run(*rt.tts_model, req.text, backend);
            if (!res.ok) {
                if (!protocol::emit(protocol::tts_error(req.seq, res.error))) {
                    running = false;
                    shutdown_reason = static_cast<int>(ShutdownReason::StdoutClosed);
                    return false;
                }
                continue;
            }
            // (c) Enqueue the mono buffer for playback and report the AUDIO
            // duration (out_ms = sample_count / rate), not wall time.
            AudioBufferPcm buf;
            buf.sample_rate = res.sample_rate;
            buf.samples = res.samples;
            const int64_t out_ms = buf.sample_rate > 0
                                       ? static_cast<int64_t>(buf.samples.size()) * 1000 /
                                             buf.sample_rate
                                       : 0;
            if (playback_ok) {
                if (!pb.queue().enqueue(std::move(buf))) {
                    std::cerr << "daemon: tts seq=" << req.seq
                              << ": playback queue full — audio dropped\n";
                }
            } else {
                std::cerr << "daemon: tts seq=" << req.seq << ": no playback device — "
                          << res.samples.size() << " samples (" << out_ms
                          << " ms) synthesized but not played\n";
            }
            // (d) tts.done.
            if (!protocol::emit(protocol::tts_done(req.seq, out_ms))) {
                running = false;
                shutdown_reason = static_cast<int>(ShutdownReason::StdoutClosed);
                return false;
            }
        }
    };

    // Drains agent (pi) commands queued by the PiAgent reader thread (T12).
    // Reply -> run TTS on THIS thread (ISC-A-1) and emit agent.reply.done;
    // with --no-speak, emit agent.reply.done {spoken:false} without speaking.
    // Error -> emit agent.error and keep going (the daemon stays up; NDJSON
    // mode continues). Returns false when stdout is closed. Never throws.
    const auto drain_agent_commands = [&]() -> bool {
        for (;;) {
            AgentCommand cmd;
            {
                std::lock_guard<std::mutex> lock(agent_mutex);
                if (agent_commands.empty()) {
                    return true;
                }
                cmd = std::move(agent_commands.front());
                agent_commands.pop_front();
            }
            if (cmd.kind == AgentCommand::Kind::Error) {
                // A rejected prompt settles its turn (no reply will come):
                // decrement so the shutdown wait doesn't hang on it.
                if (cmd.prompt_rejected && outstanding_replies > 0) {
                    --outstanding_replies;
                }
                std::cerr << "daemon: agent error: " << cmd.error << "\n";
                if (!protocol::emit(protocol::agent_error(cmd.error))) {
                    running = false;
                    shutdown_reason = static_cast<int>(ShutdownReason::StdoutClosed);
                    return false;
                }
                continue;
            }
            if (outstanding_replies > 0) {
                --outstanding_replies;
            }
            if (getenv("PERSONA_DEBUG_TIMELINE")) {
                std::cerr << "dbg: agent reply seq=" << cmd.seq << " text='" << cmd.text
                          << "'\n";
            }
            if (cmd.text.empty() || cfg.no_speak) {
                // Nothing to speak (empty reply) or told not to: report done
                // without audio. chars still reflects the reply length.
                if (!protocol::emit(protocol::agent_reply_done(cmd.seq, cmd.text, false))) {
                    running = false;
                    shutdown_reason = static_cast<int>(ShutdownReason::StdoutClosed);
                    return false;
                }
                continue;
            }
            if (!rt.tts_model) {
                if (!protocol::emit(protocol::agent_error(
                        "TTS model not loaded for agent reply — install it with:  " +
                        install_hint(rt.tts_family, cfg.tts_package)))) {
                    running = false;
                    shutdown_reason = static_cast<int>(ShutdownReason::StdoutClosed);
                    return false;
                }
                continue;
            }
            const TtsSession::Result res = TtsSession::run(*rt.tts_model, cmd.text, backend);
            if (!res.ok) {
                if (!protocol::emit(protocol::agent_error(
                        std::string("tts failed for agent reply: ") + res.error))) {
                    running = false;
                    shutdown_reason = static_cast<int>(ShutdownReason::StdoutClosed);
                    return false;
                }
                continue;
            }
            AudioBufferPcm buf;
            buf.sample_rate = res.sample_rate;
            buf.samples = res.samples;
            const int64_t out_ms = buf.sample_rate > 0
                                       ? static_cast<int64_t>(buf.samples.size()) * 1000 /
                                             buf.sample_rate
                                       : 0;
            if (playback_ok) {
                if (!pb.queue().enqueue(std::move(buf))) {
                    std::cerr << "daemon: agent reply seq=" << cmd.seq
                              << ": playback queue full — audio dropped\n";
                }
            } else {
                std::cerr << "daemon: agent reply seq=" << cmd.seq
                          << ": no playback device — " << res.samples.size() << " samples ("
                          << out_ms << " ms) synthesized but not played\n";
            }
            if (!protocol::emit(protocol::agent_reply_done(cmd.seq, cmd.text, true))) {
                running = false;
                shutdown_reason = static_cast<int>(ShutdownReason::StdoutClosed);
                return false;
            }
        }
    };

    // Drains endpointer intents -> SttSession lifecycle + NDJSON out. Returns
    // false when stdout is closed (triggers graceful shutdown).
    const auto drain_intents = [&]() -> bool {
        for (;;) {
            const Endpointer::Intent intent = ep.next_intent();
            if (intent == Endpointer::Intent::None) {
                return true;
            }
            if (getenv("PERSONA_DEBUG_TIMELINE")) {
                std::cerr << "dbg: drain intent=" << static_cast<int>(intent)
                          << " seq=" << ep.seq() << " state=" << static_cast<int>(ep.state())
                          << " chunk_start=" << chunk_start << "\n";
            }
            if (intent == Endpointer::Intent::BeginUtterance) {
                first_partial_pending = true;
                const int seq = ep.seq();
                const int64_t t_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - daemon_started).count();
                if (!protocol::emit(protocol::speech_start(seq, t_ms))) {
                    running = false;
                    shutdown_reason = static_cast<int>(ShutdownReason::StdoutClosed);
                    return false;
                }
                try {
                    const auto t_begin0 = std::chrono::steady_clock::now();
                    stt.begin_utterance(backend);
                    const auto t_begin1 = std::chrono::steady_clock::now();
                    // Feed the pre-roll (leading silence + the onset chunk,
                    // which the live loop skipped because the session did not
                    // exist yet when the VAD fired). All buffered chunks are
                    // full 512-sample chunks, so positions stay contiguous.
                    if (getenv("PERSONA_DEBUG_TIMELINE")) {
                        std::cerr << "dbg: begin seq=" << seq << " speech_start=" << ep.start_sample()
                                  << " preroll_base=" << preroll_base
                                  << " preroll_samples=" << preroll.size()
                                  << " init_ms="
                                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                                         t_begin1 - t_begin0)
                                         .count()
                                  << "\n";
                    }
                    if (!preroll.empty()) {
                        const int64_t base = preroll_base;
                        for (size_t i = 0;
                             i + static_cast<size_t>(kChunk) <= preroll.size();
                             i += static_cast<size_t>(kChunk)) {
                            const std::vector<float> slice(
                                preroll.begin() + static_cast<long>(i),
                                preroll.begin() + static_cast<long>(i + static_cast<size_t>(kChunk)));
                            stt.feed(slice, base + static_cast<int64_t>(i));
                        }
                        preroll.clear();
                    }
                } catch (const std::exception& ex) {
                    // ASR session failed to start: surface speech.error and drop
                    // the utterance (back to Idle; the next SpeechStart retries).
                    protocol::emit(protocol::speech_error(
                        seq, std::string("stt: begin_utterance failed: ") + ex.what()));
                    ep.abort_utterance();
                    stt.prepare(backend);  // idempotent; re-arm the fast path
                }
            } else {  // EndUtterance
                const int seq = ep.seq();
                const int64_t s0 = ep.start_sample();
                const int64_t s1 = ep.end_sample();
                final_received = false;
                final_text.clear();
                stt.end_utterance();  // fires on_final / on_error synchronously
                // Pre-create the next utterance's session while the pipeline
                // is idle (the ~0.7 s init must not land on the next VAD
                // onset; see the startup prepare above).
                stt.prepare(backend);
                if (final_received) {
                    const int64_t duration_ms = (s1 - s0) * 1000 / kRate;
                    if (getenv("PERSONA_DEBUG_TIMELINE")) {
                        std::cerr << "dbg: final seq=" << seq << " s0=" << s0 << " s1=" << s1
                                  << " dur_ms=" << duration_ms << "\n";
                    }
                    if (!protocol::emit(protocol::speech_final(seq, final_text, duration_ms))) {
                        running = false;
                        shutdown_reason = static_cast<int>(ShutdownReason::StdoutClosed);
                        return false;
                    }
                    // --agent pi: hand the utterance to the pi child. The
                    // prompt is written from this (pipeline) thread — the
                    // single writer — and agent.sent confirms the handoff
                    // (the reply arrives asynchronously via the reader
                    // thread -> agent_commands). Store the seq BEFORE the
                    // write so a reply landing right after maps to this
                    // utterance. Empty transcripts are not submitted (nothing
                    // to ask).
                    if (pi && pi->running() && !final_text.empty()) {
                        pending_reply_seq.store(seq);
                        pi->submit_prompt(seq, final_text);
                        ++outstanding_replies;
                        if (!protocol::emit(protocol::agent_sent(seq, final_text))) {
                            running = false;
                            shutdown_reason = static_cast<int>(ShutdownReason::StdoutClosed);
                            return false;
                        }
                    }
                }
                // Barge-in (SpeechStart during finalize): reopen the next
                // utterance immediately — its BeginUtterance intent is drained
                // on the next loop iteration (queue depth 1).
                ep.reopen_pending();
            }
        }
    };

    // ---- pipeline loop (ALL engine calls on this thread) ----
    std::vector<float> pending;
    pending.reserve(static_cast<size_t>(kChunk));
    int64_t pos = 0;
    bool fixture_eof = false;

    // Mic-idle watchdog state (live-mic mode only; a fixture simply ends, and
    // the watchdog must never fire on the fixture path). last_samples_at is
    // refreshed on every source() call that yields samples.
    auto last_samples_at = std::chrono::steady_clock::now();
    bool mic_idle_logged = false;
    auto mic_idle_last_log = std::chrono::steady_clock::time_point{};
    // VAD-degraded state: one higher-level log per broken episode (the episode
    // ends when VadSession's failure counter resets on a successful feed — see
    // VadSession::feed's restart state machine).
    bool vad_degraded_logged = false;

    // Pipeline-loop liveness: any unexpected exception escaping the
    // non-throwing contracts (a source()/ring/session-adjacent failure) is
    // caught here, logged with a clear reason, and turned into a graceful
    // shutdown (finalize + vad.finish() below still run) — the daemon must
    // never just stop pumping samples silently. (The loop was NOT previously
    // wrapped; the only inner try/catches are drain_intents's stt.begin_utterance
    // and the playback open, so nothing is double-wrapped.)
    try {
        while (running.load() && g_terminate_signal == 0) {
            // Pending tts commands are handled here, between audio chunks, so the
            // TTS session calls stay serialized on this thread (ISC-A-1).
            if (!drain_tts_commands()) {
                break;  // stdout closed
            }
            // Pending agent (pi) commands likewise (replies run TTS here; errors
            // surface as agent.error).
            if (!drain_agent_commands()) {
                break;  // stdout closed
            }
            std::vector<float> tmp(static_cast<size_t>(kChunk));
            const size_t n = source(tmp.data(), tmp.size());
            if (n == 0) {
                if (flags.fixture && fixture_cursor >= fixture.size()) {
                    fixture_eof = true;
                    break;  // fixture consumed -> shutdown after finalizing
                }
                // Mic-idle watchdog: no samples for a while means the PA input
                // stream has likely died (USB suspend, device error, another app
                // grabbed the mic) — log once at kMicIdleWarn, re-log every
                // kMicIdleRelog, checking the PA stream state on each fire. This
                // branch runs only when the mic is silent (fixture mode is
                // excluded), so the PA health calls stay out of the hot path.
                if (!flags.fixture) {
                    const auto now = std::chrono::steady_clock::now();
                    const auto idle = now - last_samples_at;
                    if (idle >= kMicIdleWarn &&
                        (!mic_idle_logged || now - mic_idle_last_log >= kMicIdleRelog)) {
                        mic_idle_logged = true;
                        mic_idle_last_log = now;
                        const bool pa_active = cap->stream_active();
                        const std::string pa_host_err = cap->stream_host_error();
                        std::cerr
                            << "daemon: WARNING mic source idle for "
                            << std::chrono::duration_cast<std::chrono::seconds>(idle).count()
                            << "s — no samples received; input stream may have died "
                               "(device busy? USB suspend?) [pa active="
                            << (pa_active ? "yes" : "no")
                            << (pa_host_err.empty() ? "" : ", host err: " + pa_host_err)
                            << "]\n";
                    }
                }
                std::this_thread::sleep_for(kPollInterval);
                continue;
            }
            last_samples_at = std::chrono::steady_clock::now();
            mic_idle_logged = false;
            pending.insert(pending.end(), tmp.begin(), tmp.begin() + static_cast<long>(n));

            while (pending.size() >= static_cast<size_t>(kChunk) && running.load() &&
                   g_terminate_signal == 0) {
                // silero requires full 512-sample contiguous chunks: take exactly
                // kChunk samples per feed.
                const std::vector<float> chunk(pending.begin(),
                                               pending.begin() + static_cast<long>(kChunk));
                pending.erase(pending.begin(), pending.begin() + static_cast<long>(kChunk));

                chunk_start = pos;
                const int64_t chunk_end = pos + kChunk;

                push_preroll(chunk, pos);
                vad.feed(chunk, pos);
                // VAD health: surface a higher-level, once-per-episode log when
                // the VAD session has degraded (>= 3 consecutive feed failures —
                // VadSession already attempted its one restart). Re-armed when
                // the counter resets on a successful feed.
                if (!vad_degraded_logged && vad.consecutive_failures() >= 3) {
                    vad_degraded_logged = true;
                    std::cerr << "daemon: endpointing degraded — VAD failing (see vad: logs)\n";
                } else if (vad_degraded_logged && vad.consecutive_failures() == 0) {
                    vad_degraded_logged = false;
                    std::cerr << "daemon: endpointing recovered — VAD feeding again\n";
                }
                if (ep.audio_active()) {
                    stt.feed(chunk, pos);
                }
                ep.tick(chunk_end);
                pos = chunk_end;

                if (!drain_intents()) {
                    break;
                }
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "daemon: pipeline error: " << ex.what()
                  << " — shutting down (reason=pipeline-error)\n";
        running = false;
        shutdown_reason = static_cast<int>(ShutdownReason::PipelineError);
    }

    if (g_terminate_signal != 0) {
        shutdown_reason = static_cast<int>(ShutdownReason::Signal);
    }

    // Process any tts commands queued during the last audio iteration (e.g.
    // the fixture's final chunks) unless the user asked to stop — a stop
    // means exit, not "synthesize first".
    if (running.load() && g_terminate_signal == 0) {
        drain_tts_commands();
    }

    // ---- graceful shutdown: finalize any open utterance, then exit ----
    if (fixture_eof) {
        // Feed any leftover (< 512) trailing samples as a zero-padded chunk so
        // the VAD sees the true tail of the fixture (silero needs full chunks).
        if (!pending.empty()) {
            pending.resize(static_cast<size_t>(kChunk), 0.0f);
            chunk_start = pos;
            push_preroll(pending, pos);
            vad.feed(pending, pos);
            // Do NOT feed the zero-padded flush chunk to the ASR — its padding
            // would sit in the final window as fake silence. The VAD sees it
            // (so a trailing SpeechEnd still fires); the ASR's audio ends at
            // the last real sample of the fixture.
            ep.tick(pos + kChunk);
            pos += kChunk;
            drain_intents();
        }
        if (ep.state() == Endpointer::State::Speaking) {
            // The fixture's speech ran to the very end without a SpeechEnd
            // (e.g. trailing silence < vad_min_silence_ms): force-finalize so
            // the transcript is not lost.
            ep.force_finalize(pos - static_cast<int64_t>(pending.size()));
            drain_intents();
        }
        // A user stop that landed during fixture processing must WIN over the
        // fixture's EOF (the stdin thread set StdinStop while the pipeline
        // was busy inferring) — otherwise a `| head -1`/stop test could report
        // audio-fixture-eof instead of stdin-stop depending on timing.
        if (shutdown_reason.load() == static_cast<int>(ShutdownReason::None)) {
            shutdown_reason = static_cast<int>(ShutdownReason::FixtureEof);
        }
    } else if (ep.state() == Endpointer::State::Speaking) {
        // stop / signal / stdout-closed mid-utterance: graceful finalize.
        ep.force_finalize(pos);
        drain_intents();
    }

    vad.finish();  // closes the VAD stream; emits a closing SpeechEnd if open
    if (cap) {
        cap->stop();
    }
    if (playback_ok) {
        // Stop the output stream (buffered audio is flushed/discarded). The
        // stream must be closed before PortAudio's process-wide teardown runs
        // at exit — Playback::~Playback (Pa_StopStream + Pa_CloseStream)
        // runs when this function returns, i.e. before the static PaGlobal
        // guards reach their last Pa_Terminate.
        pb.stop();
    }

    // Drain agent (pi) commands that arrived during shutdown (errors, late
    // replies). On the fixture-EOF path, WAIT for in-flight replies first —
    // the stub answers ~100 ms after each prompt, a real pi steers over
    // seconds — so agent.reply.done lands before shutdown. The wait is
    // bounded (30 s — generous for a loaded machine) and ends early when the
    // child dies (running()==false — no reply can arrive; the EOF's on_error
    // surfaces as agent.error via the final drain). Stop/signal paths skip
    // the wait (stop means exit, not "finish the turn").
    if (running.load() && g_terminate_signal == 0 && pi && pi->running() &&
        outstanding_replies > 0) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (outstanding_replies > 0 && pi->running() &&
               std::chrono::steady_clock::now() < deadline) {
            if (!drain_agent_commands()) {
                break;  // stdout closed
            }
            std::this_thread::sleep_for(kPollInterval);
        }
    }
    drain_agent_commands();

    protocol::emit(protocol::shutdown(shutdown_reason_str(
        static_cast<ShutdownReason>(shutdown_reason.load()))));
    stdin_thread.detach();  // may be blocked on getline; the process exits anyway
    if (pi) {
        // SIGTERM the child (SIGKILL after a short grace), join the reader,
        // reap — before the process exits so no zombie is left behind. The
        // reader is joined here, so no callback can fire after this point.
        pi->shutdown();
    }
    // Hard exit: skip ALL static destructors (PortAudio, ggml/Vulkan device
    // teardown, OpenMP runtime). Any of them can block for seconds on a wedged
    // audio backend or GPU, hanging the process after the final shutdown line
    // (observed: Ctrl+C left the daemon alive until the next stdin event).
    // Everything needed is already done — stdout is flushed per line, the pi
    // child is SIGTERMed and reaped above; the OS reclaims the rest.
    std::_Exit(0);
}

}  // namespace persona
