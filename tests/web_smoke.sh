#!/usr/bin/env bash
# T10 web smoke tests: WS audio in/out, page serving, connection lifecycle,
# ephemeral port, and daemon stay-up on disconnect.
#
# Each test runs the daemon in --web mode against a loopback WS client
# (tests/ws_client.py), one daemon at a time (never parallel). Deterministic:
# no mic, no network beyond 127.0.0.1.
#
# Run from the repo root:  tests/web_smoke.sh   (or PERSONA_BIN=<bin>).

set -u

TIMEOUT=${T10_DAEMON_TIMEOUT:-300}
BIN=${PERSONA_BIN:-./result/bin/persona}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
STUB="$ROOT/tests/pi_stub.sh"
WAV="$ROOT/testdata/hello_hello.wav"
MODELS="$ROOT/models"
WS_CLIENT="$ROOT/tests/ws_client.py"
ASR_PKG=qwen3_asr_0_6b_q8_0

fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }

# Stops the daemon gracefully (SIGTERM), waits for the shutdown line, then
# reaps. Returns 0 on clean exit.
daemon_stop() {
    local pid=$1 out=$2
    kill -TERM "$pid" 2>/dev/null || true
    # Wait for shutdown line (up to 10 s)
    local deadline=$(($(date +%s) + 10))
    while [ $(date +%s) -lt "$deadline" ]; do
        grep -q '"type":"shutdown"' "$out" 2>/dev/null && break
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.2
    done
    wait "$pid" 2>/dev/null || true
    local rc=$?
    # Exit code 0 or 143 (128+SIGTERM, expected when we sent the signal)
    [ "$rc" -eq 0 ] || [ "$rc" -eq 143 ] || fail "daemon: exit code $rc (want 0 or 143)"
    return 0
}

# ============================================================
# Test 1: GET / returns the page (200 + marker), /nope returns 404.
# ============================================================
test_page() {
    local port=18765 out err rc
    out=$(mktemp); err=$(mktemp)
    timeout "$TIMEOUT" "$BIN" daemon --web --mic none --web-port "$port" \
        --models-root "$MODELS" >"$out" 2>"$err" &
    local dpid=$!
    sleep 2
    python3 "$WS_CLIENT" page 127.0.0.1 "$port"
    rc=$?
    daemon_stop "$dpid" "$out"
    rm -f "$out" "$err"
    [ "$rc" -eq 0 ] || fail "test_page: page check failed"
    echo "  test_page: ok"
}

# ============================================================
# Test 2: connect -> hello event; second connection rejected; first stays up.
# ============================================================
test_hello() {
    local port=18766 out err rc
    out=$(mktemp); err=$(mktemp)
    timeout "$TIMEOUT" "$BIN" daemon --web --mic none --web-port "$port" \
        --models-root "$MODELS" >"$out" 2>"$err" &
    local dpid=$!
    sleep 2
    python3 "$WS_CLIENT" hello 127.0.0.1 "$port"
    rc=$?
    daemon_stop "$dpid" "$out"
    rm -f "$out" "$err"
    [ "$rc" -eq 0 ] || fail "test_hello: hello check failed"
    echo "  test_hello: ok"
}

# ============================================================
# Test 3: stream hello_hello.wav via WS with stub pi + --no-speak.
# The WS client sees events while connected; the daemon stdout
# sees 2 finals + 2 agent events (utterance 2 is force-finalized
# on disconnect).  We check the daemon's stdout after the client
# disconnects (the force-finalize fires then).
# ============================================================
test_stream() {
    local port=18767 out err rc
    out=$(mktemp); err=$(mktemp)
    PERSONA_PI_BIN="$STUB" timeout "$TIMEOUT" "$BIN" daemon --agent pi --no-speak \
        --web --mic none --web-port "$port" --models-root "$MODELS" \
        --asr-package "$ASR_PKG" >"$out" 2>"$err" &
    local dpid=$!
    sleep 2
    # Connect, stream wav, collect events, close.
    python3 "$WS_CLIENT" stream 127.0.0.1 "$port" "$WAV"
    rc=$?
    [ "$rc" -eq 0 ] || fail "test_stream: ws_client failed"
    # Wait for the daemon to process the force-finalize (utterance 2).
    local deadline=$(($(date +%s) + 30))
    local finals=0
    while [ $(date +%s) -lt "$deadline" ]; do
        finals=$(grep -c '"type":"speech.final"' "$out" 2>/dev/null)
        [ "$finals" -ge 2 ] && break
        kill -0 "$dpid" 2>/dev/null || break
        sleep 0.5
    done
    [ "$finals" -ge 2 ] || fail "test_stream: $finals speech.final on stdout (want >=2)"
    local sent=$(grep -c '"type":"agent.sent"' "$out" 2>/dev/null)
    local done=$(grep -c '"type":"agent.reply.done"' "$out" 2>/dev/null)
    [ "$sent" -ge 2 ] || fail "test_stream: $sent agent.sent (want >=2)"
    [ "$done" -ge 2 ] || fail "test_stream: $done agent.reply.done (want >=2)"
    daemon_stop "$dpid" "$out"
    rm -f "$out" "$err"
    echo "  test_stream: ok (finals=$finals sent=$sent done=$done)"
}

# ============================================================
# Test 4: disconnect -> daemon stays up, reconnect accepted.
# ============================================================
test_reconnect() {
    local port=18768 out err rc
    out=$(mktemp); err=$(mktemp)
    timeout "$TIMEOUT" "$BIN" daemon --web --mic none --web-port "$port" \
        --models-root "$MODELS" >"$out" 2>"$err" &
    local dpid=$!
    sleep 2
    python3 "$WS_CLIENT" reconnect 127.0.0.1 "$port"
    rc=$?
    [ "$rc" -eq 0 ] || fail "test_reconnect: reconnect check failed"
    daemon_stop "$dpid" "$out"
    rm -f "$out" "$err"
    echo "  test_reconnect: ok"
}

# ============================================================
# Test 5: --web-port 0 -> ephemeral port logged to stderr.
# ============================================================
test_ephemeral() {
    local out err rc port
    out=$(mktemp); err=$(mktemp)
    timeout "$TIMEOUT" "$BIN" daemon --web --mic none --web-port 0 \
        --models-root "$MODELS" >"$out" 2>"$err" &
    local dpid=$!
    sleep 2
    # Parse the actual port from the stderr log line.
    port=$(grep -oP 'listening on 127\.0\.0\.1:\K\d+' "$err" | head -1)
    if [ -z "$port" ]; then
        fail "test_ephemeral: could not parse port from stderr"
        daemon_stop "$dpid" "$out" 2>/dev/null
        rm -f "$out" "$err"
        return
    fi
    # Verify the page is served on that port.
    python3 "$WS_CLIENT" page 127.0.0.1 "$port"
    rc=$?
    [ "$rc" -eq 0 ] || fail "test_ephemeral: page on port $port failed"
    daemon_stop "$dpid" "$out"
    rm -f "$out" "$err"
    echo "  test_ephemeral: ok (port=$port)"
}

# ============================================================
# Audio-out test (gated on TTS model presence). hello_hello.wav finalizes
# utterance 1 naturally while the client is connected (the 1s+ gap between
# its two utterances exceeds vad_min_silence_ms; hello.wav's 0.44 s trailing
# silence does not, so it stays open until a disconnect force-finalize).
# --no-interrupt keeps the barge-in flush from racing the reply audio away:
# the reply for utterance 1 is spoken (TTS -> 24 kHz PCM16 frames) and must
# arrive at the client before it disconnects.
# ============================================================
test_audio_out() {
    # Check that pocket_tts is installed.
    if ! "$BIN" models list 2>/dev/null | grep -q 'pocket_tts.*yes'; then
        echo "  test_audio_out: SKIP (pocket_tts not installed)"
        return
    fi
    local port=18769 out err rc out_py
    out=$(mktemp); err=$(mktemp)
    PERSONA_PI_BIN="$STUB" timeout "$TIMEOUT" "$BIN" daemon --agent pi --no-interrupt \
        --web --mic none --web-port "$port" --models-root "$MODELS" \
        --asr-package "$ASR_PKG" >"$out" 2>"$err" &
    local dpid=$!
    sleep 2
    out_py=$(python3 "$WS_CLIENT" audio-out 127.0.0.1 "$port" "$WAV")
    rc=$?
    echo "$out_py" | grep -q 'binary_bytes=[1-9]' \
        || fail "test_audio_out: no binary audio frames arrived ($out_py)"
    [ "$rc" -eq 0 ] || fail "test_audio_out: audio-out check failed"
    daemon_stop "$dpid" "$out"
    rm -f "$out" "$err"
    echo "  test_audio_out: ok ($(echo "$out_py" | sed 's/RESULT audio-out OK //'))"
}

echo "T10 web smoke:"
test_page
test_hello
test_stream
test_reconnect
test_ephemeral
test_audio_out

if [ "$fails" -eq 0 ]; then
    echo "ALL T10 SMOKE TESTS PASSED"
    exit 0
fi
echo "$fails test(s) FAILED" >&2
exit 1