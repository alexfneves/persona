#pragma once

// NDJSON protocol between the daemon and an external agent (Decision 1): one
// JSON object per line on stdout (daemon -> agent) and stdin (agent ->
// daemon), UTF-8, with an explicit flush after every emitted line. The OUT
// helpers build nlohmann::json objects with the exact field shapes from
// plan.md "Protocol (v1)"; protocol::emit is the single stdout writer (called
// from the pipeline thread only) so stdout stays pure NDJSON — all logs go to
// stderr.
//
// OUT (daemon -> agent):
//   ready          {"type":"ready","asr":..,"tts":..,"vad":..,"rate":16000,
//                   "asr_package":..,"tts_package":..,"backend":..}  (T13)
//                   (tts_package only when tts is loaded; "agent":"pi" only
//                   with --agent pi)
//   speech.start   {"type":"speech.start","seq":n,"t_ms":..}
//   speech.partial {"type":"speech.partial","seq":n,"text":..}
//   speech.final   {"type":"speech.final","seq":n,"text":..,"empty":bool,
//                   "duration_ms":..,"chars":..}
//   speech.error   {"type":"speech.error","seq":n,"error":..}
//   tts.start      {"type":"tts.start","seq":n}   (T11)
//   tts.done       {"type":"tts.done","seq":n,"out_ms":..}  (audio duration, T11)
//   tts.error      {"type":"tts.error","seq":n,"error":..}
//   agent.sent     {"type":"agent.sent","seq":n,"text":..}   (--agent pi, T12)
//   agent.reply.done {"type":"agent.reply.done","seq":n,"chars":..,"spoken":bool}
//   agent.error    {"type":"agent.error","error":..}
//   shutdown       {"type":"shutdown","reason":..}
//
// IN (agent -> daemon):
//   stop           {"type":"stop"}
//   tts            {"type":"tts","text":..,"seq":n}   (handled in T11)

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace persona::protocol {

// ---- OUT ----
nlohmann::json ready(const std::string& asr, const std::string& tts,
                     const std::string& vad, int rate,
                     const std::string& asr_package,
                     const std::string& tts_package,
                     const std::string& backend,
                     const std::string& agent = std::string());
nlohmann::json speech_start(int seq, int64_t t_ms);
nlohmann::json speech_partial(int seq, const std::string& text);
nlohmann::json speech_final(int seq, const std::string& text, int64_t duration_ms);
nlohmann::json speech_error(int seq, const std::string& error);
nlohmann::json tts_start(int seq);
nlohmann::json tts_done(int seq, int64_t out_ms);
nlohmann::json tts_error(int seq, const std::string& error);
nlohmann::json agent_sent(int seq, const std::string& text);
nlohmann::json agent_reply_done(int seq, const std::string& text, bool spoken);
nlohmann::json agent_error(const std::string& error);
nlohmann::json shutdown(const std::string& reason);

// Writes one JSON object as a single NDJSON line on stdout and flushes.
// Returns false when stdout is closed/broken (the daemon ignores SIGPIPE, so
// the failure surfaces here and callers trigger graceful shutdown — ISC-12).
// Called from the pipeline thread only.
bool emit(const nlohmann::json& obj);

// ---- IN ----
enum class CommandKind { Stop, Tts, Unknown };

struct Command {
    CommandKind kind = CommandKind::Unknown;
    std::string type;   // raw "type" field ("" when the line is not JSON)
    std::string text;   // tts commands
    int seq = 0;        // tts commands
    std::string error;  // parse diagnostics for the stderr log (ISC-11)
};

// Parses one stdin line. Never throws; malformed lines yield kind Unknown with
// a description in Command::error — the daemon logs it to stderr and keeps
// running (ISC-11).
Command parse_command(const std::string& line);

}  // namespace persona::protocol
