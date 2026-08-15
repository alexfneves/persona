# Code Review — persona voice daemon (first full review)

**Reviewed:** full master tree (20+ commits, T0–T14) against `plan.md` (Decisions 1–9, ISC-1..14, ISC-A-1..3), `todos.md`, and `AGENTS.md`. First review of the whole codebase.
**Reviewed by:** reviewer agent (no code modified).
**Date:** 2026-08-15
**Verdict:** **NEEDS CHANGES** — no P0 found, but two P1s (one user-facing functional gap, one shutdown-stall) and several P2s worth fixing before "leave it running" trust.

---

## Summary

The codebase is in remarkably good shape for a 20+-commit un-reviewed build-out: the endpointer state machine is small and correct for its scope, the threading model (all engine calls on one pipeline thread; PortAudio callbacks touch only lock-free structures) is consistently respected, the daemon stdout is provably pure NDJSON, and the process/child handling (fork+exec, pipes, reaping, signal safety) is careful and well-documented. I re-ran the deterministic acceptance suite on current master and **all ISC claims reproduce** (details below). The findings below are mostly about things the docs over-claim, counters that can leak on pi failure paths, and malformed-input robustness.

## ISC verification (ran on master, all green)

| Criterion | Result |
|---|---|
| ISC-1 `nix build .#persona` | ✅ store build present; binary works (`result/bin/persona`) |
| ISC-2 `selftest` | ✅ `registered_loaders=4 … silero_vad_loaded=yes … OK` |
| ISC-3 `listen hello.wav` | ✅ `Hello, world. This is a test.` |
| ISC-4 `models install` | ✅ (T5 real download; resume/idempotence re-verified in todos; code reviewed) |
| ISC-5 daemon → `speech.final` | ✅ fixture run: 1 final + correct text, exit 0 |
| ISC-6 two utterances → 2 finals | ✅ `hello_hello.wav` → exactly 2 `speech.final`, exit 0 |
| ISC-7 `{"type":"stop"}` → exit 0 | ✅ `{"reason":"stdin-stop"}`, exit 0 |
| ISC-8 `persona tts` | ✅ 84524 B valid WAV (24 kHz mono), listen roundtrip clean |
| ISC-9 >30 s cap | ✅ `tone_long.wav --utt-cap-s 3` → force-finalize at 3008 ms, `empty:true` |
| ISC-10 empty transcript | ✅ `"empty":true` emitted, daemon stays up |
| ISC-11 malformed stdin | ✅ `not json` + unknown type logged to stderr, daemon continues, stop still processed |
| ISC-12 stdout closed | ✅ `| head -1` → graceful exit 0 |
| ISC-13 package selection | ✅ ready echoes `asr_package`/`backend`; `--asr-package bogus` fails fast with info hint; `--asr-family bogus` fails fast with search hint |
| ISC-14 `--agent pi` vs stub | ✅ full `agent_pi_smoke.sh` suite: happy/garbage/crlf/kill/regression all pass |
| ISC-A-1 single-thread engine calls | ✅ code-verified: VAD/STT/TTS sessions only touched on the pipeline thread; pi-reader and stdin threads marshal via queues |
| ISC-A-2 no blocking in PA callbacks | ✅ capture callback pushes only; playback callback pops + linear-resamples only |
| ISC-A-3 pin, no HEAD tracking | ✅ `flake.lock`: audiocpp `ref: release-0.6` |

---

## Findings

### [P1] Daemon ASR/VAD sessions hardcode CPU — the Vulkan variant and `--backend` do not accelerate the headline path

**File:** `src/pipeline/stt.cpp:32`, `src/pipeline/vad.cpp:18` (also `src/tts.cpp:113`)
**Issue:** `SttSession::begin_utterance()` and `VadSession::start()` build `SessionOptions` with `opts.backend.type = engine::core::BackendType::Cpu` and take no backend parameter. The daemon computes the backend from `cfg.backend` (`src/daemon/daemon.cpp:226`) and passes it **only** to `TtsSession::run`. Consequences:
- `nix build .#persona-vulkan` runs ASR and VAD on CPU regardless — the two-utterance fixture endpoints at CPU speed on both builds.
- `--backend vulkan` (documented as an override) changes nothing for the ASR path; only TTS and offline `listen` honor it.
- The `ready` line reports `"backend":"vulkan"` on a Vulkan build while ASR runs CPU — misleading.
- **README.md:119-121** claims "Vulkan ≈ **2.5× faster** than CPU — the daemon's two-utterance fixture endpoints in ≈ **1.4 s vs ~10 s+** on CPU". This is not reproducible on master: `stt.cpp` has hardcoded CPU since T8 (commit 62f4d20), predating the Vulkan variant. Either the benchmark measured something else (offline listen) or a locally-patched binary.

**Suggested Fix:** plumb the parsed `BackendConfig` into `SttSession::begin_utterance()` and `VadSession::start()` (both already take an options surface; add a parameter), and have `verb_daemon` pass the cfg-derived backend. Re-measure and correct the README claim. This is also the concrete manifestation of the flagged "backend.cpp compile-time backend assumption" maintenance risk: the macro `PERSONA_DEFAULT_BACKEND` is only a proxy — the sessions must actually honor it.

### [P1] `outstanding_replies` counter leaks → 30 s shutdown stall after a pi prompt rejection / empty reply

**File:** `src/daemon/daemon.cpp:612, 754, 894-899`; `src/agent/pi_rpc.cpp:325-340`
**Issue:** `outstanding_replies` is incremented at submit (daemon.cpp:754) and decremented only when a `Reply` AgentCommand is drained (daemon.cpp:612). Two paths never decrement:
1. pi answers `{"type":"response","success":false}` → `fire_error("prompt rejected …")` → `agent.error`, child stays alive.
2. `message_end` whose `extract_message_text()` yields `""` (e.g. thinking-only reply) → `on_reply_complete` is **not** fired (`pi_rpc.cpp:338` guards `!text.empty()`).

In both cases the fixture-EOF shutdown wait (`daemon.cpp:894-899`) spins the full 30 s (`outstanding_replies > 0 && pi->running()` both stay true) before the daemon exits. Verified by tracing; a rejection is a documented failure mode ("prompt rejected" is handled in `handle_line`), so a slow 30 s exit is reachable in normal operation.

**Suggested Fix:** decrement on error paths too — either fire `on_reply_complete` with empty text on `message_end` (let the daemon emit `agent.reply.done {chars:0,spoken:false}`), or have the daemon's `on_error` handler decrement `outstanding_replies` when the error corresponds to a submitted prompt. Simplest: move the decrement to cover both kinds.

### [P2] `pending_reply_seq` single-slot mapping misattributes replies when two prompts are in flight

**File:** `src/daemon/daemon.cpp:275, 298, 752` (comment at 269-274 claims "Prompts are strictly sequential (utterances are), so this maps a reply to its utterance")
**Issue:** the mapping is a single `std::atomic<int>`, overwritten at every submit. The claim is only true if a reply always lands before the next prompt is submitted. With voice-over (a documented feature: "queues while pi is mid-turn"), the user speaks utterance 2 while pi is still replying to utterance 1 → prompt 2 is submitted, `pending_reply_seq=2`, and reply 1's `message_end` is attributed to seq 2. Two `agent.reply.done {seq:2}` with different texts, or worse, a wrapper correlating replies to utterances gets the wrong pairing. The reply *text* is still spoken in order (pi processes sequentially), so impact is limited to the seq label — but it's a genuine mislabel on a documented path.

**Suggested Fix:** keep a FIFO of outstanding seqs (the queue already serializes); pop the head seq when a Reply is drained instead of reading a single atomic. Utterances are strictly sequential, so a small deque is unambiguous.

### [P2] WAV reader: out-of-bounds read on a crafted `fmt` chunk

**File:** `src/audio/wav.cpp:126-130`
**Issue:** `read_bytes(in, fmt.data(), chunk_size)` reads the claimed chunk size, then `le16(fmt.data() + 14)` reads bytes 14-15 unconditionally. A malformed WAV whose `fmt ` chunk claims `chunk_size < 16` (6 bytes is enough to pass `le16(fmt.data())` and `le16(fmt.data()+2)` reads for format/channels) makes `fmt.data()+14` read past the end of the heap vector — UB. Reachable from `persona listen <file>` and `daemon --audio-fixture`. Local file, read-only, likely garbage-not-crash, but it's provable UB on attacker-controllable input in a daemon that is supposed to keep running.

**Suggested Fix:** check `chunk_size >= 16` before parsing the fmt fields (skip/error otherwise). Also consider bounding `data_chunk.resize(chunk_size)` for a sanity cap.

### [P2] `try_load` silently swallows the engine load error

**File:** `src/model/registry.cpp:74` (`catch (const std::exception&) { return nullptr; }`)
**Issue:** any `registry.load` failure — including a **corrupt or partially-downloaded GGUF** — is reduced to `nullptr`, and the caller prints "ASR model not loaded … install it with: persona models install …". The install hint is actively misleading when the model *is* installed but broken (e.g. truncated by an interrupted download that resumed into a size-matching-but-corrupt file, or a wrong-variant dir). The real engine error (why the load failed) is discarded, which will cost someone hours debugging "why does it keep telling me to install".

**Suggested Fix:** log the engine exception to stderr inside the catch (keep returning null for the soft-fail contract), or return the error string on `Runtime` for the verbs to surface. The T5 downloader already "trusts size match" per the plan's risk register, so a size-correct-but-corrupt file is a realistic outcome.

### [P2] `reply_buffer_` grows unbounded across the whole daemon session

**File:** `src/agent/pi_rpc.cpp:325` (`reply_buffer_ += delta;` — never reset; `message_end` does not clear it)
**Issue:** every `text_delta` of every reply across the daemon's lifetime is appended to a string that is only ever read for debug logging ("v1: informational"). For a daemon meant to be "left running", this is unbounded memory growth proportional to total reply volume. Small per reply, but it is a pure leak with a trivial fix.

**Suggested Fix:** clear `reply_buffer_` when `message_end` arrives (or drop the accumulation entirely until `agent.partial` is built).

### [P2] Maintenance: model-selection / default-package logic duplicated in three places

**File:** `src/model/registry.cpp` (`resolve_model_selection`), `src/models.cpp` (`default_package`, `install_dir_for`, `verb_install`'s inline package pick), `src/model/download.cpp` (`package_file_target` vs the inline `rel` computation in `install_package`)
**Issue:** "default package" resolution and "install target path" computation exist independently in registry, models verbs, and downloader. A future spec-shape change (e.g. a new defaulting rule) must be applied in all three or behavior silently diverges (T4's `default`-may-be-absent case already forced one divergence). Purely a maintainability hazard — nothing is wrong today.

**Suggested Fix:** move default-package selection and `package_file_target` into `catalog.h/.cpp` and have all three callers use them.

### [P3] Fixture-EOF force-finalize duration is short by up to one chunk

**File:** `src/daemon/daemon.cpp:856`
**Issue:** after the trailing `<512` leftover samples are zero-padded to a full chunk, `pending.size()` is 512, so `force_finalize(pos - pending.size())` sets the end sample to the *start* of the leftover (the pre-flush `pos`), understating `duration_ms` by up to 511 samples (32 ms) when the fixture ends mid-chunk while speaking. Transcript is preserved — only the duration label is off. Cosmetic.

**Suggested Fix:** capture the leftover length before resizing (`pos - leftover_n`), or pass the pre-flush `pos`.

### [P3] `vad.finish()` can queue an `EndUtterance` that is never drained at shutdown

**File:** `src/daemon/daemon.cpp:871` (after the last `drain_intents()`), `src/pipeline/vad.cpp:47-50` (finish emits closing `on_speech_end`)
**Issue:** if the endpointer is in `Speaking` when `vad.finish()` runs (only reachable via a pending-barge-in reopen — `on_vad_start` while `Finalizing`, which needs a SpeechEnd+SpeechStart inside one 32 ms VAD chunk, so very contrived), the closing `on_speech_end` transitions to `Finalizing` and queues an `EndUtterance` that nothing drains; the reopened ASR session is destroyed without `finish_stream()`, losing that transcript. Structurally real, practically near-unreachable.

**Suggested Fix:** call `drain_intents()` once more after `vad.finish()` (or have shutdown force-finalize against the endpointer state directly).

### [P3] stdout-closed detection latency in mic mode

**File:** `src/daemon/daemon.cpp:553-558` (and emit sites)
**Issue:** stdout closure is only detected on the next `emit`. In mic mode with a dead wrapper and silence, the daemon keeps capturing indefinitely (ISC-12 is verified for the fixture path, which emits promptly). A long-idle daemon whose consumer died is a silent resource leak until the next utterance or signal.

**Suggested Fix:** periodically probe with an empty check, or (simpler, documented) accept the current behavior for v1 — but state it in the README.

### [P3] Lax numeric flag parsing

**File:** `src/config.cpp` (`std::stoi`/`std::stod` on `--utt-cap-s`, `--vad-min-silence-ms`, etc.)
**Issue:** `std::stoi("30abc")` parses 30 and does not throw, so `--utt-cap-s 30abc` silently becomes 30. Harmless for a CLI, but it means malformed input is silently accepted.

**Suggested Fix:** verify the whole token was consumed (`pos == size` after `stoi`/`stod`).

### [P3] Downloader path checks are lexical only (no symlink resolution)

**File:** `src/model/download.cpp` (`is_safe_relative`, `uninstall_family` mismatch check)
**Issue:** install writes and uninstall's `remove_all` validate the *lexical* path stays under `models_root` but do not resolve symlinks. A symlink planted inside the models root (e.g. `models/Qwen3-ASR-1.7B-GGUF -> /some/other/dir`) redirects writes (install) — and `std::filesystem::remove_all` on a directory whose *contents* include symlinks removes the links, not targets, so the blast radius is limited. The models root is a same-user local directory (no privilege boundary crossed), so this is defense-in-depth only — but it is exactly the "symlink traversal" the review brief asked about, and a hostile spec or shared models root would hit it.

**Suggested Fix:** cheap: resolve `weakly_canonical()` on the target and require it to be under `weakly_canonical(models_root)` in `uninstall_family` (already does a prefix check) and before `create_directories`/`fopen` in `download_file`.

### [P3] `--backend nope` validated after the model load, not before

**File:** `src/daemon/daemon.cpp:208` (`make_runtime` first) then `:226` (`parse_backend`)
**Issue:** T13 bills this as "fail fast", but a bad `--backend` value still loads qwen3 (seconds) before failing. `resolve_model_selection` correctly fails fast for bad family/package; the backend check should move before `make_runtime` for consistency.

### [P3] Shutdown-path PortAudio exceptions skip the `shutdown` line and flip the exit code

**File:** `src/daemon/daemon.cpp:875-884` (`cap->stop()` / `pb.stop()` can throw), `src/audio/playback.cpp:82-85`
**Issue:** `Pa_StopStream` failure propagates out of `verb_daemon` → `main` prints `persona: …` and returns 1, so the final `{"type":"shutdown"}` NDJSON line is never emitted. Rare (PA stop failures), but the protocol contract is "shutdown line always last".

**Suggested Fix:** wrap the stop calls in try/catch, log to stderr, continue to emit `shutdown`.

### [P3] Stale comment in `stt.h` (0.5 s window vs the actual 1.0 s)

**File:** `src/pipeline/stt.h:40, 62`
**Issue:** the header says "0.5 keeps partials snappy" / "0.5 s windows complete", but `src/pipeline/stt.cpp:10-13` sets `kStreamWindowSeconds = "1.0"` and the cpp comment explains 0.5 s hallucinates (verified in T8). The header is the first thing a reader sees and directly contradicts the code.

**Suggested Fix:** update the header comment to 1.0 s with the rationale.

### [P3] Minor
- `src/daemon/daemon.cpp:143` per-loop `std::vector<float> tmp(kChunk)` allocation (and `pending.erase` memmoves) — fine at 16 kHz, but a reused buffer + cursor would remove both.
- `src/daemon/daemon.cpp` stdin `{"type":"tts"}` followed immediately by `{"type":"stop"}` before the pipeline drains → tts command dropped with **no ack line** (documented in T11 as "stop means exit", but the wrapper cannot distinguish dropped from never-sent; a `tts.error` ack would help).
- `src/agent/pi_rpc.cpp:40` — child inherits persona's `SIGPIPE=SIG_IGN` across exec (SIG_IGN persists); a stub/pi that relies on default SIGPIPE behavior gets EPIPE instead of death. Harmless for real pi, worth a comment.
- `flake.nix` raw clang++ link line: the 7-archive order is fragile-by-hand, but the `release-0.6` pin plus link-time undefined-symbol failures for a missing Vulkan archive make drift fail loudly rather than silently. The `find . -name '*.a'` copy in `audiocpp-lib` means a future audio.cpp bump that adds an archive silently leaves it out of persona's link line until the linker complains — acceptable, but worth a comment near the link line.

## What's Good

- **The endpointer** is the right size for the problem: a 4-state machine with explicit intent queue, correct handling of the force-finalize/stale-SpeechEnd interaction (Idle ignores stray ends), documented queue-depth-1 barge-in, and no hidden timers beyond the cap. The `on_vad_start`-during-`Finalizing` → `pending_` → `reopen_pending()` cycle is coherent.
- **The threading contract is real, not aspirational** — I traced every engine call site: VAD/STT/TTS sessions are only ever touched on the pipeline thread; the PA capture/playback callbacks only push/pop lock-free structures with no allocation; the pi reader and stdin threads marshal through mutex+deque queues. ISC-A-1/A-2 hold.
- **The pi child handling is unusually careful** for a v1: parent-side PATH resolution, fork+exec with only async-signal-safe calls in the child, `close_range`, stderr errno reporting on exec failure, SIGTERM→SIGKILL escalation, reaping, idempotent shutdown, and the reader-thread EOF path that keeps the daemon alive (agent.error) when the child dies.
- **Malformed-input resilience is consistent**: non-throwing feed contracts in all three session wrappers, broken-session guards, `parse_command` never throws, garbage on pi stdout skipped, `emit` failure → graceful shutdown. The daemon genuinely cannot be crashed by its stdin or by the agent child.
- **Determinism where it matters**: TTS seed pinned, VAD chunk policy honored, spec parsing defensive against the 6 specs missing `package_defaults.download`.
- **The `.part`/resume/idempotent install design** with manifest-driven `models list` and the per-file installed-check (T13's catch of the same-`target_directory` variant bug) is solid.

---

*Report produced by reviewer subagent; no code was modified. All commands re-run against the built `result/bin/persona` on master (commit 669afe4).*

---

## Fix pass (fix-review-p1)

**Branch:** `fix-review-p1` (worker subagent). **Date:** 2026-08-15.
**Scope:** the two P1s + the three listed QUICK P2s; all other P2/P3s are
deferred and remain open (marked below). All work verified with
`devenv test` (CPU build, green) and a `nix build .#persona-vulkan` run.

### Resolved findings (fixed in fix-review-p1)

| Finding | Status | Fix (file:line on fix-review-p1) |
|---|---|---|
| **P1-1** daemon ASR/VAD hardcode CPU | ✅ **fixed in fix-review-p1** | `src/backend.h:27-29` + `src/backend.cpp:30-44` (new `backend_name()`); `src/pipeline/stt.cpp:24-40` (`begin_utterance(BackendConfig)`, `opts.backend = backend`); `src/pipeline/vad.cpp:18-35` (same); `src/pipeline/stt.h:50-57`, `src/pipeline/vad.h:42-53`; `src/daemon/daemon.cpp:222-230` (backend parsed before sessions) + `:611` (`stt.begin_utterance(backend)`) + `:697` (`vad.start(vad_opts, backend)`); `src/listen.cpp:139-153` (streaming path parses + passes backend). Per-session backend debug lines under `PERSONA_DEBUG_TIMELINE`: `dbg: vad session backend=…` / `dbg: asr session backend=…`. Also fixed the stale 0.5 s comments in `src/pipeline/stt.h:40-41,66` (P3). |
| **P1-2** `outstanding_replies` leaks on rejection / empty reply → 30 s shutdown stall | ✅ **fixed in fix-review-p1** | `src/agent/pi_rpc.h:44-57` (new `on_prompt_rejected` event; `on_reply_complete` documented to fire for empty text); `src/agent/pi_rpc.cpp:346-358` (`response success:false` → `on_prompt_rejected`), `:326-336` (`message_end` fires `on_reply_complete` even with empty text); `src/daemon/daemon.cpp:294-298` (`AgentCommand::prompt_rejected`), `:349-364` (daemon `on_prompt_rejected` handler enqueues tagged error), `:640-659` (drain decrements on rejected-prompt errors; reply decrement clamped). Verified: rejection path exits in ~2 s, empty-reply path in ~2-3 s (was ~32 s). New tests: `tests/pi_stub.sh` `PERSONA_STUB_REJECT=1` / `PERSONA_STUB_EMPTY_REPLY=1` knobs; `tests/agent_pi_smoke.sh` `test_reject` / `test_empty_reply` (wall-time bound < 25 s proves no leak). |
| **P2-1** WAV `fmt`-chunk OOB read | ✅ **fixed in fix-review-p1** | `src/audio/wav.cpp:123-139` — `chunk_size < 16` → clean `wav: malformed fmt chunk` error; `:142-151` — data chunk > 1 GiB → clean `wav: data chunk too large` error (bounds the uint32_t allocation). Verified with crafted WAVs (`/tmp/bad_fmt.wav` size-6 fmt, `/tmp/big_data.wav` 2 GiB data claim): both error cleanly, no UB. |
| **P2-2** `reply_buffer_` unbounded growth | ✅ **fixed in fix-review-p1** | `src/agent/pi_rpc.cpp:22-25` (`kReplyBufferCap = 64 KiB`), `:304-309` (deltas dropped past the cap), `:330` (`reply_buffer_.clear()` on `message_end`). |
| **P2-3** `try_load` swallows engine errors | ✅ **fixed in fix-review-p1** | `src/model/registry.cpp:74-84` — the engine exception is logged to stderr (`registry: load failed for <dir> (<family>): <what>`); still returns null for the soft-fail contract, so the install hint remains only when the model genuinely isn't installed. |

### Deferred findings (documented notes — not fixed in this pass)

- **P2** `pending_reply_seq` single-slot mapping misattributes replies when two prompts are in flight — deferred (needs a FIFO of seqs; utterance ordering makes the practical impact label-only).
- **P2** model-selection / default-package logic duplicated in three places — deferred (refactor, no behavior change).
- **P3** fixture-EOF force-finalize duration off by up to one chunk — deferred (cosmetic, transcript preserved).
- **P3** `vad.finish()` can queue an `EndUtterance` never drained at shutdown — deferred (near-unreachable).
- **P3** stdout-closed detection latency in mic mode — deferred (documented v1 behavior; README states the fixture path).
- **P3** lax numeric flag parsing (`stoi("30abc")` → 30) — deferred.
- **P3** downloader path checks lexical only (no symlink resolution) — deferred (defense-in-depth).
- **P3** `--backend nope` validated after the model load — deferred (fail-fast timing; the backend is now parsed before any session is created, so a bad value fails before inference, just after model load).
- **P3** shutdown-path PortAudio exceptions skip the `shutdown` line — deferred.
- **P3 minor** per-loop `tmp`/`pending.erase` allocation; stdin `tts`-then-`stop` ack gap; SIGPIPE-inherit comment; flake link-line comment — deferred.

### P1-1 acceptance: daemon fixture wall time (before/after)

Command: `persona daemon --mic none --audio-fixture testdata/hello_hello.wav --models-root models`
(wall time of the whole process, `date +%s%N`; AMD Strix Halo, qwen3_asr 1.7B).

| Run | `.#persona-vulkan` default (vulkan) | `.#persona-vulkan --backend cpu` | CPU-only `.#persona` |
|---|---|---|---|
| 1 | 3.45 s | 7.31 s | 7.05 s |
| 2 | 3.31 s | 7.33 s | 7.23 s |
| 3 | 3.33 s | 7.28 s | — |
| **avg** | **3.36 s** | **7.31 s** | **7.14 s** |

Speedup: **≈ 2.2×** (vulkan vs cpu). The README's old claim ("≈ 1.4 s vs
~10 s+", 2.5×) was not reproducible on master because ASR/VAD never left
the CPU; it is now updated to the measured numbers (`README.md:117-123`).

Note: before this fix, the Vulkan binary ran VAD+ASR on CPU (the hardcoded
`BackendType::Cpu` in `stt.cpp`/`vad.cpp`), so a Vulkan run of the fixture
took ≈ 7.3 s — identical to CPU. After the fix the default run is ≈ 3.4 s.

### P1-2 verification

- `tests/agent_pi_smoke.sh` (all 7 tests incl. the two new ones) passes on the CPU build.
- Rejection path (`PERSONA_STUB_REJECT=1`, 0.6B package): daemon run = **~2.5 s**, emits `agent.error` `"prompt rejected: stub rejection"`, exits 0 with `audio-fixture-eof` — no 30 s stall.
- Empty-reply path (`PERSONA_STUB_EMPTY_REPLY=1`): daemon run = **~2.4 s**, emits `agent.reply.done {chars:0,spoken:false}`, exits 0.
- Kill/child-death path (existing `test_kill`): unchanged and green (child-death `on_error` correctly does NOT settle the counter — `pi->running()` is false by then, so the shutdown wait exits immediately).
- Full `devenv test`: **ALL SMOKE TESTS PASSED**.
