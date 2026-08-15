#!/usr/bin/env bash
# Model-free smoke subset for `nix flake check` (checks.<system>.smoke in
# flake.nix). The check derivation runs inside the nix build sandbox: no
# network, and `models/` (gitignored) is NOT part of the flake source — so
# everything that needs a downloaded model is skipped here. The full
# model-dependent suite is `bash tests/smoke.sh` / `devenv test` on a machine
# with models installed.
#
# The check derivation passes the nix-built binary via PERSONA_BIN
# ($persona/bin/persona, a store path). Run manually from the repo root:
#   PERSONA_BIN=result/bin/persona bash tests/flake_check.sh

set -uo pipefail
cd "$(dirname "$0")/.." || exit 1
BIN=${PERSONA_BIN:-result/bin/persona}

echo "── selftest (loader list) ───────────────────────────────────"
"$BIN" selftest > /tmp/persona_check_selftest.out || { echo "FAIL: selftest"; exit 1; }
grep -q 'silero_vad_loaded=yes' /tmp/persona_check_selftest.out \
  || { echo "FAIL: silero_vad not loaded"; exit 1; }

echo "── selftest --vad (silero_vad endpointing, synthetic audio) ─"
"$BIN" selftest --vad > /tmp/persona_check_vad.out || { echo "FAIL: selftest --vad"; exit 1; }
grep -q 'selftest --vad: OK' /tmp/persona_check_vad.out \
  || { echo "FAIL: VAD endpointing assert"; exit 1; }

echo "── models search (catalog sanity, compiled-in specs) ────────"
"$BIN" models search --task tts | grep -q 'pocket_tts' \
  || { echo "FAIL: pocket_tts missing from catalog"; exit 1; }
"$BIN" models search --task asr --streaming | grep -q 'qwen3_asr' \
  || { echo "FAIL: qwen3_asr missing from streaming ASR catalog"; exit 1; }

echo "── --version ────────────────────────────────────────────────"
"$BIN" --version | grep -q 'persona' || { echo "FAIL: --version"; exit 1; }

# Model-dependent asserts (listen / daemon endpointing / pi stub) need
# downloaded models — they cannot run in the sandbox and are covered by
# tests/smoke.sh. The guard keeps this script a valid full smoke when run
# manually from a repo root that has models/.
if [ ! -d models ]; then
  echo "SKIP model-dependent asserts: no models/ dir (nix flake check sandbox)."
  echo "      Full suite: bash tests/smoke.sh  (one-time: persona models install qwen3_asr pocket_tts)"
else
  echo "── listen on fixture ────────────────────────────────────────"
  transcript=$("$BIN" listen --models-root models testdata/hello.wav) \
    || { echo "FAIL: listen"; exit 1; }
  [ "$transcript" = "Hello, world. This is a test." ] \
    || { echo "FAIL: unexpected transcript: '$transcript'"; exit 1; }

  echo "── daemon endpointing (hello_hello: expect 2 speech.final) ──"
  finals=$("$BIN" daemon --models-root models --mic none --audio-fixture testdata/hello_hello.wav 2>/dev/null \
    | grep -c '"type":"speech.final"')
  [ "$finals" = "2" ] || { echo "FAIL: got $finals speech.final (want 2)"; exit 1; }

  echo "── pi stub (--agent pi, ISC-14) ────────────────────────────"
  if [ -x tests/agent_pi_smoke.sh ]; then
    PERSONA_BIN="$BIN" bash tests/agent_pi_smoke.sh || { echo "FAIL: pi stub smoke"; exit 1; }
  fi
fi

echo "FLAKE CHECK SMOKE: PASSED"
