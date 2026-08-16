#!/bin/sh
# Fake `pi --mode rpc` responder for scripted tests (T12). Reads JSONL
# commands from stdin and, for every `prompt` command, prints the minimal RPC
# event stream the daemon's reply path consumes:
#   {"type":"response","command":"prompt","success":true}
#   message_update text_start/text_delta/text_end
#   message_end with a full AgentMessage
#   turn_end with the same AgentMessage (the authoritative reply boundary)
# It also exits 0 on a `{"type":"stop"}`-style command (not used by the daemon
# — shutdown is SIGTERM — but kept for standalone use).
#
# POSIX sh on purpose: the daemon spawns this script via its shebang, and the
# daemon's environment (nix) has no `bash` on PATH — /usr/bin/env bash would
# fail and the stub would never run. /bin/sh exists everywhere (NixOS symlinks
# it to bash). Delays are busy-waits with builtins only: forking an external
# `sleep` would leave a child holding the stdout pipe open past this script's
# death, which breaks the parent's EOF detection.
#
# Test knobs (env):
#   PERSONA_STUB_GARBAGE=1  emit one non-JSON line before the response (the
#                           daemon must skip it and still produce the reply).
#   PERSONA_STUB_CRLF=1     terminate every emitted line with \r\n (the daemon
#                           must strip the \r and still parse each line).
#   PERSONA_STUB_SLOW=1     wait ~1.8 s before each response (lets a test kill
#                           the stub while a reply is pending).
#   PERSONA_STUB_HANG_AFTER_FIRST=1
#                           reply to the FIRST prompt, then busy-wait forever
#                           (the kill test: kills the stub after a delivered
#                           reply while a later reply is pending).
#   PERSONA_STUB_REJECT=1   answer every prompt with
#                           {"type":"response","success":false} and NO
#                           message_end (the prompt-rejection path: the daemon
#                           must settle its outstanding-reply accounting).
#   PERSONA_STUB_EMPTY_REPLY=1
#                           send message_end + turn_end with EMPTY text (a
#                           thinking-only reply: the daemon must settle the
#                           turn with a stderr log and NO agent.reply.done).
#
# Framing is LF (matching pi's docs/rpc.md); shell printf writes directly (no
# stdio buffering), so every line is flushed to the pipe as written.

set -u

REPLIED_FIRST=0

# Busy-wait `$1` iterations with builtins only (~0.125 s per 20000 on typical
# hosts).
pauses() {
    _i=0
    while [ "$_i" -lt "$1" ]; do :; _i=$((_i + 1)); done
}

slow_wait() {
    if [ "${PERSONA_STUB_SLOW:-}" = "1" ]; then
        pauses 300000   # ~1.8 s
    fi
}

hang_forever() {
    while :; do :; done
}

emit() {
    if [ "${PERSONA_STUB_CRLF:-}" = "1" ]; then
        printf '%s\r\n' "$1"
    else
        printf '%s\n' "$1"
    fi
}

reply() {
    slow_wait
    if [ "${PERSONA_STUB_GARBAGE:-}" = "1" ]; then
        printf '%s\n' 'not json'
    fi
    if [ "${PERSONA_STUB_REJECT:-}" = "1" ]; then
        # A rejected prompt: response success:false and NOTHING else — no
        # message_end follows. The daemon must surface agent.error and settle
        # the turn without waiting for a reply that never comes.
        emit '{"type":"response","command":"prompt","success":false,"error":"stub rejection"}'
        return
    fi
    emit '{"type":"response","command":"prompt","success":true}'
    emit '{"type":"message_update","assistantMessageEvent":{"type":"text_start","contentIndex":0}}'
    if [ "${PERSONA_STUB_EMPTY_REPLY:-}" = "1" ]; then
        # Thinking-only reply: message_end + turn_end with empty text (no
        # text_delta). The daemon must settle the turn with a stderr log and
        # NO agent.reply.done.
        emit '{"type":"message_end","message":{"role":"assistant","content":[{"type":"thinking","text":"hmm"}]}}'
        emit '{"type":"turn_end","message":{"role":"assistant","content":[{"type":"thinking","text":"hmm"}]},"toolResults":[]}'
        return
    fi
    emit '{"type":"message_update","assistantMessageEvent":{"type":"text_delta","contentIndex":0,"delta":"Hello from stub"}}'
    emit '{"type":"message_update","assistantMessageEvent":{"type":"text_end","contentIndex":0,"content":"Hello from stub"}}'
    emit '{"type":"message_end","message":{"role":"assistant","content":[{"type":"text","text":"Hello from stub"}]}}'
    # turn_end is the authoritative reply boundary (the daemon fires its
    # reply completion here, NOT on message_end — which pi emits once per
    # assistant sub-message and would otherwise duplicate the done).
    emit '{"type":"turn_end","message":{"role":"assistant","content":[{"type":"text","text":"Hello from stub"}]},"toolResults":[]}'
}

# The daemon writes LF-only lines, so no \r stripping is needed on input.
while IFS= read -r line; do
    case "$line" in
        *'"type":"stop"'*)
            exit 0
            ;;
        *'"type":"prompt"'*)
            pauses 20000   # ~0.1 s: let the daemon finish submitting/reading
            if [ "${PERSONA_STUB_HANG_AFTER_FIRST:-}" = "1" ] && [ "$REPLIED_FIRST" -eq 1 ]; then
                hang_forever  # a later reply never arrives
            fi
            reply
            REPLIED_FIRST=1
            ;;
        *)
            # Unknown command: ignore (mirrors pi ignoring what it doesn't
            # understand).
            ;;
    esac
done

exit 0
