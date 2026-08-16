#include "protocol/ndjson.h"

#include <iostream>
#include <stdexcept>

namespace persona::protocol {

namespace {

// Length of a string in UTF-8 code points (the speech.final "chars" field).
// Every non-continuation byte (0b10xxxxxx) begins a code point, so counting
// them is correct for well-formed UTF-8 (transcripts come from the model).
std::size_t utf8_len(const std::string& s) {
    std::size_t n = 0;
    for (const unsigned char c : s) {
        if ((c & 0xC0) != 0x80) {
            ++n;
        }
    }
    return n;
}

// agent.reply.done "text" is a DISPLAY preview of the reply (the wrapper
// shows what the agent said; TTS already spoke the full text before the done
// is emitted). Cap it at kReplyTextMaxCp code points so a very long reply
// (a 30B model can ramble) never produces an unbounded NDJSON line; a
// truncated preview gets a trailing U+2026 HORIZONTAL ELLIPSIS marker. The
// "chars" field still reports the FULL reply length.
constexpr std::size_t kReplyTextMaxCp = 500;

std::string truncate_reply_text(const std::string& s) {
    if (utf8_len(s) <= kReplyTextMaxCp) {
        return s;
    }
    std::string out;
    out.reserve(s.size() + 3);
    std::size_t cp = 0;
    for (const unsigned char c : s) {
        if ((c & 0xC0) != 0x80) {  // start of a code point
            if (cp == kReplyTextMaxCp) {
                break;
            }
            ++cp;
        }
        out += static_cast<char>(c);
    }
    out += "\xE2\x80\xA6";  // U+2026 HORIZONTAL ELLIPSIS
    return out;
}

}  // namespace

nlohmann::json ready(const std::string& asr, const std::string& tts,
                     const std::string& vad, int rate,
                     const std::string& asr_package,
                     const std::string& tts_package,
                     const std::string& backend,
                     const std::string& agent) {
    nlohmann::json j = {{"type", "ready"}, {"asr", asr}, {"tts", tts},
                        {"vad", vad}, {"rate", rate}};
    // T13: echo the resolved family's package ids (empty only when the model
    // is not loaded — "tts":"none" keeps its meaning) and the compiled-in
    // backend (PERSONA_DEFAULT_BACKEND; --backend is an override, ready shows
    // what the binary was built for).
    if (!asr_package.empty()) {
        j["asr_package"] = asr_package;
    }
    if (!tts_package.empty()) {
        j["tts_package"] = tts_package;
    }
    j["backend"] = backend;
    if (!agent.empty()) {
        j["agent"] = agent;  // {"agent":"pi"} only with --agent pi (T12)
    }
    return j;
}

nlohmann::json speech_start(int seq, int64_t t_ms) {
    return {{"type", "speech.start"}, {"seq", seq}, {"t_ms", t_ms}};
}

nlohmann::json speech_partial(int seq, const std::string& text) {
    return {{"type", "speech.partial"}, {"seq", seq}, {"text", text}};
}

nlohmann::json speech_final(int seq, const std::string& text, int64_t duration_ms) {
    return {{"type", "speech.final"},
            {"seq", seq},
            {"text", text},
            {"empty", text.empty()},
            {"duration_ms", duration_ms},
            {"chars", utf8_len(text)}};
}

nlohmann::json speech_error(int seq, const std::string& error) {
    return {{"type", "speech.error"}, {"seq", seq}, {"error", error}};
}

nlohmann::json tts_start(int seq) {
    return {{"type", "tts.start"}, {"seq", seq}};
}

nlohmann::json tts_done(int seq, int64_t out_ms) {
    return {{"type", "tts.done"}, {"seq", seq}, {"out_ms", out_ms}};
}

nlohmann::json tts_error(int seq, const std::string& error) {
    return {{"type", "tts.error"}, {"seq", seq}, {"error", error}};
}

nlohmann::json agent_sent(int seq, const std::string& text) {
    return {{"type", "agent.sent"}, {"seq", seq}, {"text", text}};
}

nlohmann::json agent_reply_done(int seq, const std::string& text, bool spoken) {
    return {{"type", "agent.reply.done"},
            {"seq", seq},
            {"chars", utf8_len(text)},
            {"text", truncate_reply_text(text)},
            {"spoken", spoken}};
}

nlohmann::json agent_error(const std::string& error) {
    return {{"type", "agent.error"}, {"error", error}};
}

nlohmann::json shutdown(const std::string& reason) {
    return {{"type", "shutdown"}, {"reason", reason}};
}

bool emit(const nlohmann::json& obj) {
    std::cout << obj.dump() << '\n' << std::flush;
    return static_cast<bool>(std::cout);
}

Command parse_command(const std::string& line) {
    Command cmd;
    if (line.empty()) {
        cmd.error = "empty line";
        return cmd;
    }
    try {
        const nlohmann::json obj = nlohmann::json::parse(line);
        if (!obj.is_object()) {
            cmd.error = "not a JSON object";
            return cmd;
        }
        cmd.type = obj.value("type", std::string());
        if (cmd.type == "stop") {
            cmd.kind = CommandKind::Stop;
        } else if (cmd.type == "tts") {
            cmd.kind = CommandKind::Tts;
            cmd.text = obj.value("text", std::string());
            cmd.seq = obj.value("seq", 0);
        } else {
            cmd.error = "unknown type '" + cmd.type + "'";
        }
    } catch (const std::exception& ex) {
        cmd.error = std::string("invalid JSON: ") + ex.what();
    }
    return cmd;
}

}  // namespace persona::protocol