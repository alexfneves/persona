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
#   PERSONA_STUB_LOG=<path> append every received command line to <path> (the
#                           test assertion hook for interrupt tests).
#   PERSONA_STUB_TURN_END_AFTER_ABORT=1
#                           after an abort ack, also emit a trailing EMPTY-text
#                           turn_end (the swallowed-reply path: the daemon must
#                           discard it and settle its reply accounting).
#   PERSONA_STUB_SILENT_AFTER_ABORT=1
#                           after an abort ack, emit NOTHING else (the
#                           pathological case: no turn_end, no agent_settled).
#   PERSONA_STUB_DEFER_FIRST_REPLY=1
#                           defer the FIRST prompt's reply until the abort
#                           arrives (flushed before the abort ack). The
#                           interrupt test uses this: reply 1 is provably still
#                           in flight when utterance 2 finalizes — pipe ordering
#                           (daemon writes abort BEFORE prompt 2) makes this
#                           deterministic, with no wall-clock timing.
#
# On an abort command the stub emits the ack
#   {"type":"response","command":"abort","success":true}
# and (unless PERSONA_STUB_SILENT_AFTER_ABORT=1) a trailing agent_settled;
# with PERSONA_STUB_TURN_END_AFTER_ABORT=1 a trailing empty-text turn_end
# precedes it (exercises the swallow path).
#
# Framing is LF (matching pi's docs/rpc.md); shell printf writes directly (no
# stdio buffering), so every line is flushed to the pipe as written.

set -u

REPLIED_FIRST=0
DEFERRED=0

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
    if [ -n "${PERSONA_STUB_LOG:-}" ]; then
        printf '%s\n' "$line" >> "$PERSONA_STUB_LOG"
    fi
    case "$line" in
        *'"type":"stop"'*)
            exit 0
            ;;
        *'"type":"abort"'*)
            # The interrupted turn's reply was deferred (DEFER_FIRST_REPLY):
            # emit it BEFORE the ack so the daemon swallows it against the
            # aborted reply-FIFO front — the settled that follows must NOT
            # settle that entry first (it would then be spoken as the next
            # prompt's reply).
            if [ "${PERSONA_STUB_DEFER_FIRST_REPLY:-}" = "1" ] && [ "$DEFERRED" -eq 1 ]; then
                reply
                # NB: DEFERRED stays 1 — the deferral is one-shot (first
                # prompt only); later prompts see DEFERRED!=0 and reply
                # normally.
            fi
            emit '{"type":"response","command":"abort","success":true}'
            if [ "${PERSONA_STUB_TURN_END_AFTER_ABORT:-}" = "1" ]; then
                # A trailing EMPTY-text turn_end: the interrupted turn's reply
                # arrives after the abort — the daemon must swallow it (no
                # TTS, no agent.reply.done) and settle its accounting.
                emit '{"type":"turn_end","message":{"role":"assistant","content":[{"type":"text","text":""}]},"toolResults":[]}'
            fi
            if [ "${PERSONA_STUB_SILENT_AFTER_ABORT:-}" != "1" ]; then
                emit '{"type":"agent_settled"}'
            fi
            ;;
        *'"type":"prompt"'*)
            if [ "${PERSONA_STUB_DEFER_FIRST_REPLY:-}" = "1" ] && [ "$DEFERRED" -eq 0 ]; then
                # Defer the FIRST prompt's reply: it is flushed by the abort
                # case above. All later prompts reply normally.
                DEFERRED=1
                continue
            fi
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
