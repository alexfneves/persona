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

}  // namespace

nlohmann::json ready(const std::string& asr, const std::string& tts,
                     const std::string& vad, int rate) {
    return {{"type", "ready"}, {"asr", asr}, {"tts", tts}, {"vad", vad}, {"rate", rate}};
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