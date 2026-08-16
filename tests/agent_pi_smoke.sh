#!/usr/bin/env bash
# T12 smoke: `persona daemon --agent pi` against the pi stub (tests/pi_stub.sh).
# Covers the ISC-14 contract and the framing/robustness edge cases:
#   1. happy path      ready(agent:pi) -> speech.final -> agent.sent ->
#                      agent.reply.done {chars>0, text, spoken:false} -> exit 0
#   2. garbage line    a non-JSON line on pi's stdout is skipped (no crash)
#   3. \r\n framing    \r\n-terminated events still parse
#   4. child killed    kill -9 the stub after a delivered reply -> agent.error,
#                      daemon stays up, fixture EOF -> clean exit 0
#   5. empty reply     turn_end with no text -> stderr log, NO agent.reply.done,
#                      accounting settles (fast exit)
#   6. regression      no --agent: unchanged NDJSON (no agent field), 1 final,
#                      {"type":"stop"} -> stdin-stop exit 0, selftest OK
#   7. interrupt       hello_hello.wav + PERSONA_STUB_DEFER_FIRST_REPLY (interrupt
#                      ON by default): the abort is sent, the interrupted turn's
#                      reply is swallowed, exactly ONE agent.reply.done (the
#                      SECOND utterance's seq)
#   8. no-interrupt    --no-interrupt: no abort ever sent, two sequential
#                      replies (one per utterance)
#
# Run from the repo root:  tests/agent_pi_smoke.sh   (or PERSONA_BIN=<bin>).

set -u

# The ASR model is slow on a loaded machine; every daemon invocation gets this
# wall-clock cap (a hung daemon must fail the test, not hang it).
TIMEOUT=${T12_DAEMON_TIMEOUT:-300}

BIN=${PERSONA_BIN:-./result/bin/persona}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
STUB="$ROOT/tests/pi_stub.sh"
HELLO="$ROOT/testdata/hello.wav"
HELLO2="$ROOT/testdata/hello_hello.wav"
MODELS="$ROOT/models"

fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }

# Runs the daemon with the stub as the pi binary and asserts the NDJSON
# contract for the happy path (one hello.wav utterance, --no-speak).
test_happy() {
    local out rc sent_line done_line
    out=$(PERSONA_PI_BIN="$STUB" timeout "$TIMEOUT" "$BIN" daemon --agent pi --no-speak --mic none \
        --audio-fixture "$HELLO" --models-root "$MODELS" 2>/dev/null)
    rc=$?
    [ "$rc" -eq 0 ] || fail "happy: exit code $rc (want 0)"
    echo "$out" | grep -q '"agent":"pi"' || fail "happy: ready missing agent:pi"
    echo "$out" | grep -q '"type":"speech.final"' || fail "happy: no speech.final"
    # agent.sent must carry the exact speech.final text (hello.wav).
    sent_line=$(echo "$out" | grep '"type":"agent.sent"')
    [ -n "$sent_line" ] || fail "happy: no agent.sent"
    echo "$sent_line" | grep -q '"text":"Hello, world. This is a test."' \
        || fail "happy: agent.sent text != speech.final text"
    done_line=$(echo "$out" | grep '"type":"agent.reply.done"')
    [ -n "$done_line" ] || fail "happy: no agent.reply.done"
    echo "$done_line" | grep -q '"chars":15' || fail "happy: agent.reply.done chars != 15"
    echo "$done_line" | grep -q '"text":"Hello from stub"' \
        || fail "happy: agent.reply.done missing reply text"
    echo "$done_line" | grep -q '"spoken":false' \
        || fail "happy: agent.reply.done not spoken:false"
    echo "$out" | grep -q '"reason":"audio-fixture-eof","type":"shutdown"' \
        || fail "happy: no audio-fixture-eof shutdown"
    echo "  happy: ok ($(echo "$out" | wc -l) lines)"
}

# A garbage (non-JSON) line on pi's stdout must be skipped without a crash.
test_garbage() {
    local out rc
    out=$(PERSONA_STUB_GARBAGE=1 PERSONA_PI_BIN="$STUB" timeout "$TIMEOUT" "$BIN" daemon --agent pi \
        --no-speak --mic none --audio-fixture "$HELLO" --models-root "$MODELS" 2>/dev/null)
    rc=$?
    [ "$rc" -eq 0 ] || fail "garbage: exit code $rc (want 0)"
    echo "$out" | grep -q '"type":"agent.reply.done"' \
        || fail "garbage: agent.reply.done missing after garbage line"
    echo "  garbage: ok"
}

# \r\n-terminated events on pi's stdout must still parse (LF-only framing with
# the trailing \r stripped).
test_crlf() {
    local out rc
    out=$(PERSONA_STUB_CRLF=1 PERSONA_PI_BIN="$STUB" timeout "$TIMEOUT" "$BIN" daemon --agent pi \
        --no-speak --mic none --audio-fixture "$HELLO" --models-root "$MODELS" 2>/dev/null)
    rc=$?
    [ "$rc" -eq 0 ] || fail "crlf: exit code $rc (want 0)"
    # agent.reply.done only appears if the \r\n-terminated message_end parsed
    # (an unstripped \r would make the line invalid JSON and drop the reply).
    echo "$out" | grep -q '"type":"agent.reply.done"' \
        || fail "crlf: agent.reply.done missing (\\r\\n events not parsed)"
    echo "  crlf: ok"
}

# A REJECTED prompt (response success:false, no message_end) must surface as
# agent.error AND settle the outstanding-reply accounting — otherwise the
# fixture-EOF shutdown waits the full 30 s for a reply that will never come
# (review P1-2). Uses the fast 0.6B package so the wall-time bound is
# meaningful; a leaked counter pushes the run to ~32 s, a fixed one stays
# well under the bound.
test_reject() {
    local out rc start end elapsed
    start=$(date +%s)
    out=$(PERSONA_STUB_REJECT=1 PERSONA_PI_BIN="$STUB" timeout "$TIMEOUT" "$BIN" daemon --agent pi \
        --no-speak --mic none --audio-fixture "$HELLO" --models-root "$MODELS" \
        --asr-package qwen3_asr_0_6b_q8_0 2>/dev/null)
    rc=$?
    end=$(date +%s)
    elapsed=$((end - start))
    [ "$rc" -eq 0 ] || fail "reject: exit code $rc (want 0)"
    echo "$out" | grep -q '"error":"prompt rejected' \
        || fail "reject: no prompt-rejected agent.error"
    [ "$elapsed" -lt 25 ] || fail "reject: ${elapsed}s elapsed — the 30 s shutdown wait still leaks (want < 25 s)"
    echo "  reject: ok (${elapsed}s)"
}

# A message_end + turn_end with EMPTY text (thinking-only reply) must still
# complete the turn — settle the accounting with a stderr log and NO
# agent.reply.done (a prompt that produced no answer text must not surface a
# chars:0 done). Same wall-time bound as test_reject.
test_empty_reply() {
    local out err rc start end elapsed
    err=$(mktemp)
    start=$(date +%s)
    out=$(PERSONA_STUB_EMPTY_REPLY=1 PERSONA_PI_BIN="$STUB" timeout "$TIMEOUT" "$BIN" daemon --agent pi \
        --no-speak --mic none --audio-fixture "$HELLO" --models-root "$MODELS" \
        --asr-package qwen3_asr_0_6b_q8_0 2>"$err")
    rc=$?
    end=$(date +%s)
    elapsed=$((end - start))
    [ "$rc" -eq 0 ] || fail "empty-reply: exit code $rc (want 0)"
    echo "$out" | grep -q '"type":"agent.reply.done"' \
        && fail "empty-reply: agent.reply.done emitted for an empty reply (want none)"
    grep -q 'agent reply empty' "$err" \
        || fail "empty-reply: no stderr log for the empty reply"
    [ "$elapsed" -lt 25 ] || fail "empty-reply: ${elapsed}s elapsed — the 120 s shutdown wait still leaks (want < 25 s)"
    rm -f "$err"
    echo "  empty-reply: ok (${elapsed}s)"
}

# Interrupt (F3): with interrupt ON (default), a new utterance final while an
# agent reply is in flight supersedes that turn — pi gets an abort command, the
# interrupted turn's reply is swallowed (no agent.reply.done for it), and the
# new prompt's reply is the ONLY agent.reply.done (seq 2).
#
# Determinism: the stub defers the FIRST prompt's reply until the abort arrives
# (PERSONA_STUB_DEFER_FIRST_REPLY — pipe ordering: the daemon writes abort
# BEFORE prompt 2, so the interrupted reply provably lands after the FIFO front
# is marked aborted). A wall-clock delay (PERSONA_STUB_SLOW) is NOT enough: the
# pipeline runs ~1.2x real-time here, so the ut1-final -> ut2-final gap (~3 s)
# exceeds the stub's ~1.8 s busy-wait — the abort would never fire.
test_interrupt() {
    local out err log rc done_count done_seq
    err=$(mktemp); log=$(mktemp)
    out=$(PERSONA_STUB_LOG="$log" PERSONA_STUB_DEFER_FIRST_REPLY=1 PERSONA_PI_BIN="$STUB" \
        timeout "$TIMEOUT" "$BIN" daemon --agent pi --no-speak --mic none \
        --audio-fixture "$HELLO2" --models-root "$MODELS" \
        --asr-package qwen3_asr_0_6b_q8_0 2>"$err")
    rc=$?
    [ "$rc" -eq 0 ] || fail "interrupt: exit code $rc (want 0)"
    grep -q '"type":"abort"' "$log" \
        || fail "interrupt: stub log has no abort command (interrupt did not fire)"
    done_count=$(echo "$out" | grep -c '"type":"agent.reply.done"')
    [ "$done_count" -eq 1 ] \
        || fail "interrupt: $done_count agent.reply.done (want exactly 1 — the interrupted turn must be swallowed)"
    done_seq=$(echo "$out" | grep '"type":"agent.reply.done"' | grep -o '"seq":[0-9]*' | head -1)
    [ "$done_seq" = '"seq":2' ] \
        || fail "interrupt: agent.reply.done seq is '$done_seq' (want the SECOND utterance's, seq 2)"
    echo "$out" | grep '"type":"agent.reply.done"' | grep -q '"seq":1' \
        && fail "interrupt: agent.reply.done for the interrupted (first) utterance"
    rm -f "$err" "$log"
    echo "  interrupt: ok"
}

# --no-interrupt restores today's sequential behavior: no abort ever sent, two
# replies (one per utterance), both delivered in order (seq 1 then seq 2).
test_no_interrupt() {
    local out err log rc done_count seq1 seq2
    err=$(mktemp); log=$(mktemp)
    out=$(PERSONA_STUB_LOG="$log" PERSONA_PI_BIN="$STUB" timeout "$TIMEOUT" "$BIN" daemon \
        --agent pi --no-speak --no-interrupt --mic none --audio-fixture "$HELLO2" \
        --models-root "$MODELS" --asr-package qwen3_asr_0_6b_q8_0 2>"$err")
    rc=$?
    [ "$rc" -eq 0 ] || fail "no-interrupt: exit code $rc (want 0)"
    grep -q '"type":"abort"' "$log" \
        && fail "no-interrupt: stub log has an abort command (want none with --no-interrupt)"
    done_count=$(echo "$out" | grep -c '"type":"agent.reply.done"')
    [ "$done_count" -eq 2 ] \
        || fail "no-interrupt: $done_count agent.reply.done (want 2 sequential replies)"
    seq1=$(echo "$out" | grep '"type":"agent.reply.done"' | grep -c '"seq":1')
    seq2=$(echo "$out" | grep '"type":"agent.reply.done"' | grep -c '"seq":2')
    [ "$seq1" -eq 1 ] || fail "no-interrupt: $seq1 agent.reply.done with seq 1 (want 1)"
    [ "$seq2" -eq 1 ] || fail "no-interrupt: $seq2 agent.reply.done with seq 2 (want 1)"
    rm -f "$err" "$log"
    echo "  no-interrupt: ok"
}

# Kill the stub (kill -9) after its first reply is delivered: the daemon must
# emit agent.error, stay up, and exit 0 at fixture EOF. Uses hello_hello.wav
# (two utterances -> two prompts) with the slow stub, so the daemon is still
# waiting for reply 2 when the kill lands — deterministic.
test_kill() {
    local out err dpid spid rc r_done err_line shutdown_line
    out=$(mktemp); err=$(mktemp)
    PERSONA_STUB_HANG_AFTER_FIRST=1 PERSONA_PI_BIN="$STUB" timeout "$TIMEOUT" "$BIN" daemon --agent pi \
        --no-speak --mic none --audio-fixture "$HELLO2" --models-root "$MODELS" \
        >"$out" 2>"$err" &
    dpid=$!

    # Wait for the spawn line (the stub pid is logged there). Generous window:
    # the daemon loads models + opens playback before spawning.
    for _ in $(seq 1 600); do
        spid=$(grep -o 'spawned (pid [0-9]*)' "$err" | grep -o '[0-9]*')
        [ -n "$spid" ] && break
        kill -0 "$dpid" 2>/dev/null || break
        sleep 0.1
    done
    [ -n "$spid" ] || fail "kill: could not read stub pid from stderr"

    # Wait for the first delivered reply (utterance 1), then kill the stub.
    # The stub replies to prompt 1 then hangs forever, so reply 2 is still
    # pending (and the daemon still up) when the kill lands — deterministic.
    for _ in $(seq 1 1200); do
        if grep -q '"type":"agent.reply.done"' "$out"; then
            break
        fi
        kill -0 "$dpid" 2>/dev/null || break
        sleep 0.1
    done
    kill -9 "$spid" 2>/dev/null || true

    # The daemon must emit agent.error (child death) and exit on its own.
    for _ in $(seq 1 300); do
        if grep -q '"type":"agent.error"' "$out"; then
            break
        fi
        kill -0 "$dpid" 2>/dev/null || break
        sleep 0.1
    done
    for _ in $(seq 1 300); do
        kill -0 "$dpid" 2>/dev/null || break
        sleep 0.1
    done
    wait "$dpid"; rc=$?
    [ "$rc" -eq 0 ] || fail "kill: exit code $rc (want 0)"

    grep -q '"type":"agent.sent"' "$out" \
        || fail "kill: no agent.sent (utterance never submitted)"
    grep -q '"type":"agent.reply.done"' "$out" \
        || fail "kill: no agent.reply.done before the kill"
    grep -q '"type":"agent.error"' "$out" \
        || fail "kill: no agent.error after child death"
    grep -q '"reason":"audio-fixture-eof","type":"shutdown"' "$out" \
        || fail "kill: no audio-fixture-eof shutdown"

    # Ordering: reply.done before agent.error before shutdown (the daemon
    # stayed up and drained the error after the kill).
    r_done=$(grep -n '"type":"agent.reply.done"' "$out" | head -1 | cut -d: -f1)
    err_line=$(grep -n '"type":"agent.error"' "$out" | head -1 | cut -d: -f1)
    shutdown_line=$(grep -n '"type":"shutdown"' "$out" | head -1 | cut -d: -f1)
    if [ -n "$r_done" ] && [ -n "$err_line" ] && [ -n "$shutdown_line" ] \
        && [ "$r_done" -lt "$err_line" ] && [ "$err_line" -lt "$shutdown_line" ]; then
        :
    else
        fail "kill: wrong ordering (reply.done=$r_done error=$err_line shutdown=$shutdown_line)"
    fi
    rm -f "$out" "$err"
    echo "  kill: ok"
}

test_regression() {
    local out rc
    out=$(timeout "$TIMEOUT" "$BIN" daemon --mic none --audio-fixture "$HELLO" --models-root "$MODELS" 2>/dev/null)
    rc=$?
    [ "$rc" -eq 0 ] || fail "regression: exit code $rc (want 0)"
    echo "$out" | grep -q '"agent"' && fail "regression: ready has an agent field without --agent"
    local n
    n=$(echo "$out" | grep -c '"type":"speech.final"')
    [ "$n" -eq 1 ] || fail "regression: $n speech.final (want 1)"

    out=$(printf '{"type":"stop"}\n' | timeout "$TIMEOUT" "$BIN" daemon --mic none \
        --audio-fixture "$HELLO" --models-root "$MODELS" 2>/dev/null)
    rc=$?
    [ "$rc" -eq 0 ] || fail "regression stop: exit code $rc (want 0)"
    echo "$out" | grep -q '"reason":"stdin-stop","type":"shutdown"' \
        || fail "regression stop: no stdin-stop shutdown"

    out=$(timeout 60 "$BIN" selftest --models-root "$MODELS" 2>/dev/null)
    rc=$?
    [ "$rc" -eq 0 ] || fail "regression selftest: exit code $rc (want 0)"
    echo "$out" | grep -q 'selftest: OK' || fail "regression selftest: not OK"
    echo "  regression: ok"
}

echo "T12 agent pi smoke:"
test_happy
test_garbage
test_crlf
test_reject
test_empty_reply
test_interrupt
test_no_interrupt
test_kill
test_regression

if [ "$fails" -eq 0 ]; then
    echo "ALL T12 SMOKE TESTS PASSED"
    exit 0
fi
echo "$fails test(s) FAILED" >&2
exit 1
