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
finals=$("$BIN" daemon --mic none --audio-fixture testdata/hello_hello.wav 2>/dev/null \
  | grep -c '"type":"speech.final"')
[ "$finals" = "2" ] || { echo "FAIL: got $finals speech.final (want 2)"; exit 1; }

echo "── pi stub (--agent pi, ISC-14) ─────────────────────────────"
if [ -x tests/agent_pi_smoke.sh ]; then
  PERSONA_BIN="$BIN" bash tests/agent_pi_smoke.sh || { echo "FAIL: pi stub smoke"; exit 1; }
else
  echo "SKIP: tests/agent_pi_smoke.sh not present"
fi

echo "ALL SMOKE TESTS PASSED"
