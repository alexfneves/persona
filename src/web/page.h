#pragma once

// The embedded HTML page served by the daemon's WebServer at GET / (T9, F2).
// A self-contained browser voice client for the WS protocol (ws_server.h):
//   * URL field defaulting to ws://<current host>/, Connect/Disconnect, a
//     connection-state badge, and auto-reconnect with 1s->5s backoff (unless
//     manually disconnected).
//   * Mic capture via getUserMedia -> AudioContext -> AudioWorklet module
//     loaded from a Blob URL (ScriptProcessorNode fallback if addModule
//     fails): downmix to mono, linear-resample to 16 kHz with a fractional
//     accumulator, encode PCM16, post to the main thread -> ws.send(binary).
//   * PTT hold-button vs open-mic checkbox gates what is sent.
//   * Playback: binary frames -> ctx.createBuffer(1, n, 24000) i16->f32,
//     played through a sequentially-chained AudioBufferSourceNode queue (the
//     browser resamples 24 kHz -> context rate; no manual resample); an
//     {"type":"audio.flush"} text frame stops the current node and clears the
//     queue (barge-in).
//   * On-page log of the mirrored speech.* / agent.reply.done / error events.

#include <string>

namespace persona {

inline const std::string kPageHtml = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>persona — voice assistant</title>
<style>
  body { font-family: system-ui, sans-serif; margin: 1.5rem; max-width: 50rem; }
  h1 { font-size: 1.3rem; }
  .row { margin: 0.6rem 0; display: flex; align-items: center; gap: 0.6rem; flex-wrap: wrap; }
  #url { width: 24rem; font-size: 1rem; padding: 0.3rem; }
  button { font-size: 1rem; padding: 0.4rem 0.9rem; cursor: pointer; }
  #ptt { font-size: 1.15rem; padding: 0.7rem 2rem; }
  #ptt[disabled] { opacity: .5; cursor: not-allowed; }
  #state { font-weight: bold; padding: 0.25rem 0.6rem; border-radius: 4px; }
  .disconnected { background:#fdd; color:#a22; }
  .connecting   { background:#ffe; color:#880; }
  .connected    { background:#dfd; color:#282; }
  label { user-select: none; }
  #log { background:#0d0d12; color:#7dff8a; font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
         font-size: 0.8rem; height: 14rem; overflow-y: auto; padding: 0.5rem;
         white-space: pre-wrap; margin-top: 1rem; border-radius: 6px; }
  .l-err { color:#ff7a7a; }
</style>
</head>
<body>
<h1>persona — voice assistant</h1>

<div class="row">
  ws:// <input id="url" type="text" autocomplete="off">
  <button id="conn" type="button">Connect</button>
  <span id="state" class="disconnected">disconnected</span>
</div>

<div class="row">
  <input type="checkbox" id="mic">
  <label for="mic">Enable microphone</label>
  <input type="checkbox" id="openmic">
  <label for="openmic">Open mic (continuous)</label>
  <button id="ptt" type="button" disabled>HOLD TO TALK</button>
</div>

<div id="log"></div>

<script>
"use strict";
(function () {
  var urlField   = document.getElementById("url");
  var connBtn    = document.getElementById("conn");
  var stateBadge = document.getElementById("state");
  var micCb      = document.getElementById("mic");
  var openMicCb  = document.getElementById("openmic");
  var pttBtn     = document.getElementById("ptt");
  var logEl      = document.getElementById("log");

  urlField.value = "ws://" + location.host + "/";

  var actx = null;
  var workletNode = null;
  var spNode = null;
  var socket = null;
  var manualDisconnect = false;
  var reconnectAttempt = 0;
  var pttActive = false;
  var playQueue = [];
  var currentSource = null;

  function setState(id, label) {
    stateBadge.className = id;
    stateBadge.textContent = label;
  }
  function logLine(msg, err) {
    var e = document.createElement("div");
    if (err) e.className = "l-err";
    e.textContent = "[" + new Date().toISOString().substr(11, 8) + "] " + msg;
    logEl.appendChild(e);
    logEl.scrollTop = logEl.scrollHeight;
  }
  function ensureCtx() {
    if (!actx) actx = new (window.AudioContext || window.webkitAudioContext)();
    if (actx.state === "suspended" && actx.resume) { try { actx.resume(); } catch (e) {} }
    return actx;
  }

  // ---- mic capture: AudioWorklet (Blob URL) with ScriptProcessor fallback ----
  var WORKLET_SRC = [
    'class CaptureProc extends AudioWorkletProcessor {',
    '  constructor() {',
    '    super();',
    '    this.ratio = sampleRate / 16000;',   // input samples per 16k output sample
    '    this.nextTime = 0;',                 // fractional input position (drift-free)
    '  }',
    '  process(inputs) {',
    '    var ch = inputs[0];',
    '    if (!ch || ch.length === 0 || ch[0].length === 0) return true;',
    '    var n = ch[0].length;',
    '    var mono = new Float32Array(n);',
    '    for (var c = 0; c < ch.length; c++) {',
    '      var src = ch[c];',
    '      for (var i = 0; i < n; i++) mono[i] += src[i];',
    '    }',
    '    if (ch.length > 1) for (var i = 0; i < n; i++) mono[i] /= ch.length;',
    '    if (this.nextTime < 0) this.nextTime = 0;',
    '    var out = new Float32Array(Math.floor(n / this.ratio) + 1);',
    '    var oi = 0;',
    '    while (this.nextTime + 1 < n) {',
    '      var i0 = Math.floor(this.nextTime);',
    '      var frac = this.nextTime - i0;',
    '      var s0 = mono[i0];',
    '      var s1 = (i0 + 1 < n) ? mono[i0 + 1] : s0;',
    '      out[oi++] = s0 + (s1 - s0) * frac;',
    '      this.nextTime += this.ratio;',
    '    }',
    '    this.nextTime -= n;',
    '    if (oi === 0) return true;',
    '    var pcm = new Int16Array(oi);',
    '    for (var i = 0; i < oi; i++) {',
    '      var v = out[i] < 0 ? out[i] * 32768 : out[i] * 32767;',
    '      v = Math.max(-32768, Math.min(32767, Math.round(v)));',
    '      pcm[i] = v;',
    '    }',
    '    this.port.postMessage(pcm.buffer, [pcm.buffer]);',
    '    return true;',
    '  }',
    '}',
    'registerProcessor("persona-capture", CaptureProc);'
  ].join("\n");

  function onCaptureFrame(buf) {
    if ((pttActive || openMicCb.checked) && socket && socket.readyState === 1) {
      socket.send(buf);
    }
  }

  function startScriptProcessor(src) {
    ensureCtx();
    var ratio = actx.sampleRate / 16000;
    var nextTime = 0;
    spNode = actx.createScriptProcessor(2048, 1, 1);
    spNode.onaudioprocess = function (e) {
      var input = e.inputBuffer.getChannelData(0);
      var n = input.length;
      if (nextTime < 0) nextTime = 0;
      var out = new Float32Array(Math.floor(n / ratio) + 1);
      var oi = 0;
      while (nextTime + 1 < n) {
        var i0 = Math.floor(nextTime);
        var frac = nextTime - i0;
        var s0 = input[i0];
        var s1 = (i0 + 1 < n) ? input[i0 + 1] : s0;
        out[oi++] = s0 + (s1 - s0) * frac;
        nextTime += ratio;
      }
      nextTime -= n;
      if (oi === 0) return;
      var pcm = new Int16Array(oi);
      for (var i = 0; i < oi; i++) {
        var v = out[i] < 0 ? out[i] * 32768 : out[i] * 32767;
        v = Math.max(-32768, Math.min(32767, Math.round(v)));
        pcm[i] = v;
      }
      onCaptureFrame(pcm.buffer);
    };
    // Silent output bus so the processor is pulled; no audible feedback.
    spNode.connect(actx.destination);
    src.connect(spNode);
    pttBtn.disabled = false;
    logLine("microphone: capture started (ScriptProcessorNode fallback)");
  }

  function startCapture() {
    if (workletNode || spNode) return;
    if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
      logLine("ERROR: getUserMedia unavailable (need HTTPS or localhost).", true);
      return;
    }
    var p = getUserMediaStart();
    if (p) p.catch(function (err) {
      logLine("ERROR: microphone access denied: " + err, true);
    });
  }

  function getUserMediaStart() {
    return navigator.mediaDevices.getUserMedia({ audio: true }).then(function (stream) {
      ensureCtx();
      var src = actx.createMediaStreamSource(stream);
      var blob = new Blob([WORKLET_SRC], { type: "application/javascript" });
      var moduleUrl = URL.createObjectURL(blob);
      var p = actx.resume ? actx.resume() : Promise.resolve();
      p.then(function () {
        actx.audioWorklet.addModule(moduleUrl).then(function () {
          var node = new AudioWorkletNode(actx, "persona-capture");
          node.port.onmessage = function (e) { onCaptureFrame(e.data); };
          // Silent output bus so process() is always pulled; no audible mic.
          var gain = actx.createGain();
          gain.gain.value = 0;
          node.connect(gain);
          gain.connect(actx.destination);
          src.connect(node);
          workletNode = node;
          pttBtn.disabled = false;
          logLine("microphone: capture started (AudioWorklet)");
        }, function (err) {
          logLine("AudioWorklet addModule failed (" + err + ") — using fallback.");
          startScriptProcessor(src);
        });
      }, function () { startScriptProcessor(src); });
    });
  }

  micCb.addEventListener("change", function () {
    if (micCb.checked) startCapture();
  });

  function setPtt(v) { pttActive = v; pttBtn.classList.toggle("pressed", v); }
  pttBtn.addEventListener("mousedown", function (e) { e.preventDefault(); setPtt(true); });
  pttBtn.addEventListener("mouseup",   function (e) { e.preventDefault(); setPtt(false); });
  pttBtn.addEventListener("mouseleave",function () { setPtt(false); });
  pttBtn.addEventListener("touchstart",function (e) { e.preventDefault(); setPtt(true); });
  pttBtn.addEventListener("touchend",  function (e) { e.preventDefault(); setPtt(false); });
  pttBtn.addEventListener("touchcancel", function () { setPtt(false); });

  // ---- playback: chained AudioBufferSourceNodes (browser resamples 24k) ----
  function enqueuePlayback(arr16) {
    var n = arr16.length;
    if (n === 0) return;
    ensureCtx();
    var buf = actx.createBuffer(1, n, 24000);
    var ch = buf.getChannelData(0);
    for (var i = 0; i < n; i++) ch[i] = arr16[i] / 32768;
    playQueue.push(buf);
    pumpPlayback();
  }
  function pumpPlayback() {
    if (currentSource || playQueue.length === 0) return;
    var buf = playQueue.shift();
    var src = actx.createBufferSource();
    src.buffer = buf;
    src.connect(actx.destination);
    currentSource = src;
    src.onended = function () { currentSource = null; pumpPlayback(); };
    try { src.start(); } catch (e) { currentSource = null; }
  }
  function flushPlayback() {
    playQueue.length = 0;
    if (currentSource) {
      currentSource.onended = null;
      try { currentSource.stop(); } catch (e) {}
      currentSource = null;
    }
  }

  // ---- WebSocket ----
  var RECONNECTED_BEFORE = 0;
  function onWsMessage(data) {
    if (typeof data === "string") {
      var j;
      try { j = JSON.parse(data); } catch (e) { return; }
      var t = j.type || "";
      if (t === "hello") {
        logLine("hello: mic=" + j.mic_rate + "Hz tts=" + j.tts_rate +
                "Hz vad=" + j.vad + " asr=" + j.asr + " tts=" + j.tts);
      } else if (t === "audio.flush") {
        flushPlayback();
        logLine("audio.flush — stopping playback");
      } else if (t === "speech.start" || t === "speech.final" ||
                 t === "agent.sent" || t === "agent.reply.done" ||
                 t === "speech.error" || t === "agent.error" ||
                 t === "shutdown") {
        logLine(t + (t === "shutdown" && j.reason ? " (" + j.reason + ")" : "") +
                (j.seq !== undefined ? "  seq=" + j.seq : "") +
                (j.text ? "  " + j.text : "") +
                (j.error ? "  " + j.error : ""));
      } else if (t === "speech.partial") {
        logLine("partial  seq=" + j.seq + "  " + (j.text || ""));
      } else {
        logLine(t + (j.error ? "  " + j.error : ""));
      }
    } else {
      enqueuePlayback(new Int16Array(data));
    }
  }

  function connect() {
    manualDisconnect = false;
    connBtn.textContent = "Disconnect";
    connBtn.onclick = disconnect;
    var url = urlField.value.trim();
    if (!/^wss?:\/\//.test(url)) { logLine("ERROR: URL must start with ws:// or wss://", true); return; }
    setState("connecting", "connecting…");
    var ws;
    try { ws = new WebSocket(url); } catch (e) {
      logLine("ERROR: bad WebSocket URL: " + url, true);
      setState("disconnected", "disconnected");
      return;
    }
    socket = ws;
    ws.binaryType = "arraybuffer";
    ws.onopen = function () {
      reconnectAttempt = 0;
      setState("connected", "connected");
      logLine("connected to " + url);
    };
    ws.onmessage = function (e) { onWsMessage(e.data); };
    ws.onclose = function (e) {
      if (socket === ws) {
        socket = null;
        flushPlayback();
        setState("disconnected", "disconnected");
        connBtn.textContent = "Connect";
        connBtn.onclick = connect;
        logLine("connection closed (code " + e.code + ")");
        if (!manualDisconnect) scheduleReconnect();
      }
    };
    ws.onerror = function () { /* onclose follows */ };
  }

  function disconnect() {
    manualDisconnect = true;
    if (socket) { try { socket.close(); } catch (e) {} }
    socket = null;
    flushPlayback();
    setState("disconnected", "disconnected");
    connBtn.textContent = "Connect";
    connBtn.onclick = connect;
    logLine("disconnected (manual)");
  }

  function scheduleReconnect() {
    var delay = Math.min(5000, 1000 * Math.pow(2, reconnectAttempt));
    reconnectAttempt++;
    logLine("reconnecting in " + delay + " ms…");
    setTimeout(function () {
      if (!manualDisconnect) connect();
    }, delay);
  }

  connBtn.onclick = connect;
  connect();  // served page -> connect immediately; auto-reconnect on loss
})();
</script>
</body>
</html>
)html";

}  // namespace persona