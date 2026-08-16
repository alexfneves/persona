#!/usr/bin/env bash
# Persona smoke tests — run by `devenv test` (flake.nix `enterTest`).
#
# Deterministic: no mic, no network, no GPU. Model-dependent asserts run only
# when models/ is present (they need a one-time `persona models install ...`).
#
# Add new tests here (or add a script under tests/ and call it from here).

set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

echo "── build ─────────────────────────────────────────────────────"
nix build .#persona || { echo "FAIL: nix build .#persona"; exit 1; }
BIN=result/bin/persona

echo "── selftest (loader list) ───────────────────────────────────"
"$BIN" selftest > /tmp/persona_selftest.out || { echo "FAIL: selftest"; exit 1; }
grep -q 'silero_vad_loaded=yes' /tmp/persona_selftest.out \
  || { echo "FAIL: silero_vad not loaded"; exit 1; }

echo "── models search (catalog sanity) ───────────────────────────"
"$BIN" models search --task tts | grep -q 'pocket_tts' \
  || { echo "FAIL: pocket_tts missing from catalog"; exit 1; }

if [ ! -d models ]; then
  echo "SKIP model-dependent tests: no models/ dir."
  echo "      Install once: result/bin/persona models install qwen3_asr pocket_tts"
  echo "ALL SMOKE TESTS PASSED"
  exit 0
fi

echo "── listen on fixture ────────────────────────────────────────"
transcript=$("$BIN" listen --models-root models testdata/hello.wav) \
  || { echo "FAIL: listen"; exit 1; }
[ "$transcript" = "Hello, world. This is a test." ] \
  || { echo "FAIL: unexpected transcript: '$transcript'"; exit 1; }

echo "── daemon endpointing (hello_hello: expect 2 speech.final) ──"
finals=$("$BIN" daemon --models-root models --mic none --audio-fixture testdata/hello_hello.wav 2>/dev/null \
  | grep -c '"type":"speech.final"')
[ "$finals" = "2" ] || { echo "FAIL: got $finals speech.final (want 2)"; exit 1; }

echo "────────── T13 model selection + VAD tuning ────────────"
# Fail fast on config errors: unknown family -> search hint; unknown package
# -> info hint (both before any model load, exit 1).
if "$BIN" daemon --models-root models --mic none --audio-fixture testdata/hello.wav \
    --asr-family bogus 2>/tmp/t13_fam.err; then
  echo "FAIL: --asr-family bogus should fail fast"; exit 1
fi
grep -q 'models search --task asr' /tmp/t13_fam.err \
  || { echo "FAIL: --asr-family bogus missing search hint"; exit 1; }
if "$BIN" daemon --models-root models --mic none --audio-fixture testdata/hello.wav \
    --asr-package bogus_pkg 2>/tmp/t13_pkg.err; then
  echo "FAIL: --asr-package bogus_pkg should fail fast"; exit 1
fi
grep -q 'models info qwen3_asr' /tmp/t13_pkg.err \
  || { echo "FAIL: --asr-package bogus_pkg missing info hint"; exit 1; }

# Selection: --asr-package qwen3_asr_0_6b_q8_0 -> ready echoes the package id
# and backend, and the fixture still endpoints (2 speech.final, ISC-13). The
# 0.6B run is fast (~5 s wall vs minutes on the 1.7B default).
t13out=$("$BIN" daemon --models-root models --mic none --audio-fixture testdata/hello_hello.wav \
    --asr-package qwen3_asr_0_6b_q8_0 2>/dev/null) \
  || { echo "FAIL: daemon --asr-package qwen3_asr_0_6b_q8_0"; exit 1; }
echo "$t13out" | head -1 | grep -q '"asr_package":"qwen3_asr_0_6b_q8_0"' \
  || { echo "FAIL: ready missing 0.6B asr_package"; exit 1; }
echo "$t13out" | head -1 | grep -q '"backend":"cpu"' \
  || { echo "FAIL: ready missing backend"; exit 1; }
t13n=$(echo "$t13out" | grep -c '"type":"speech.final"')
[ "$t13n" = "2" ] || { echo "FAIL: got $t13n speech.final with 0.6B (want 2)"; exit 1; }

# VAD tuning flags accepted (fixture behavior at 0.9 is documented in
# todos.md T13: hello.wav stays ONE utterance at any threshold in (0,1] —
# real-speech onset probabilities exceed 0.9).
"$BIN" daemon --models-root models --mic none --audio-fixture testdata/hello.wav \
    --vad-threshold 0.9 --vad-min-silence-ms 800 >/tmp/t13_vad.out 2>/dev/null \
  || { echo "FAIL: daemon --vad-threshold 0.9 --vad-min-silence-ms 800"; exit 1; }
grep -q '"type":"shutdown"' /tmp/t13_vad.out \
  || { echo "FAIL: vad-flag run did not shut down cleanly"; exit 1; }

echo "── pi stub (--agent pi, ISC-14) ─────────────────────────────"
if [ -x tests/agent_pi_smoke.sh ]; then
  PERSONA_BIN="$BIN" bash tests/agent_pi_smoke.sh || { echo "FAIL: pi stub smoke"; exit 1; }
else
  echo "SKIP: tests/agent_pi_smoke.sh not present"
fi

echo "── web smoke (T10: ws:// audio in/out + page, loopback) ─────"
if [ -x tests/web_smoke.sh ]; then
  PERSONA_BIN="$BIN" bash tests/web_smoke.sh || { echo "FAIL: web smoke"; exit 1; }
else
  echo "SKIP: tests/web_smoke.sh not present"
fi

echo "ALL SMOKE TESTS PASSED"
