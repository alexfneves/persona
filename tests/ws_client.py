#!/usr/bin/env python3
"""Minimal WebSocket client for the persona web smoke tests (T10).

Python 3 stdlib only (socket + hashlib + base64): a bare RFC6455 client with
enough surface for the tests — HTTP upgrade handshake (Sec-WebSocket-Accept),
masked client->server frames, unmasked server frame parsing (with 7/16/64-bit
lengths and control frames), close handling, and WAV streaming helpers.

Modes (each prints a RESULT line; exits 0 on success, 1 on failure):
  page      <host> <port>            GET / -> 200 + page marker; /nope -> 404
  hello     <host> <port>            connect -> hello event; a second
                                     connection is rejected; the first stays up
  stream    <host> <port> <wav>      stream the wav (16k mono i16) at real-time
                                     pace, then close; prints the speech.final /
                                     agent.sent / agent.reply.done events seen
                                     over the wire while connected
  audio-out <host> <port> <wav>      like stream but waits for binary (PCM16)
                                     playback frames and reports their bytes
  reconnect <host> <port>            connect, close, reconnect, connect again
"""

import base64
import hashlib
import os
import socket
import struct
import sys
import time
import wave

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class WsError(Exception):
    pass


class Ws:
    def __init__(self, host, port, path="/", timeout=30):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(req.encode())
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise WsError("connection closed during handshake")
            data += chunk
        head, _, rest = data.partition(b"\r\n\r\n")
        lines = head.decode(errors="replace").split("\r\n")
        status = lines[0]
        accept = ""
        for ln in lines[1:]:
            k, _, v = ln.partition(":")
            if k.strip().lower() == "sec-websocket-accept":
                accept = v.strip()
        expect = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        if not status.startswith("HTTP/1.1 101") or accept != expect:
            raise WsError(f"handshake failed: {status!r} accept={accept!r}")
        self._buf = rest

    def send_binary(self, payload):
        self._send(payload, 0x2)

    def send_text(self, payload):
        self._send(payload.encode(), 0x1)

    def send_close(self, code=1000):
        self._send(struct.pack(">H", code), 0x8)

    def _send(self, payload, opcode):
        mask = os.urandom(4)
        ln = len(payload)
        if ln < 126:
            hdr = bytes([0x80 | opcode, 0x80 | ln])
        elif ln < 65536:
            hdr = bytes([0x80 | opcode, 0x80 | 126]) + struct.pack(">H", ln)
        else:
            hdr = bytes([0x80 | opcode, 0x80 | 127]) + struct.pack(">Q", ln)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(hdr + mask + masked)

    def recv_frame(self):
        """Returns (opcode, payload). None on clean EOF."""
        hdr = self._read(2)
        if hdr is None:
            return None
        opcode = hdr[0] & 0x0F
        ln = hdr[1] & 0x7F
        if ln == 126:
            ln = struct.unpack(">H", self._read(2))[0]
        elif ln == 127:
            ln = struct.unpack(">Q", self._read(8))[0]
        payload = self._read(ln)
        return (opcode, payload)

    def _read(self, n):
        while len(self._buf) < n:
            chunk = self.sock.recv(max(4096, n - len(self._buf)))
            if not chunk:
                return None
            self._buf += chunk
        out, self._buf = self._buf[:n], self._buf[n:]
        return out

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def http_get(host, port, path="/"):
    s = socket.create_connection((host, port), timeout=5)
    s.sendall(
        f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nConnection: close\r\n\r\n".encode()
    )
    data = b""
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
    s.close()
    head, _, body = data.partition(b"\r\n\r\n")
    return head.decode(errors="replace"), body


def collect_events(ws, seconds):
    """Reads text frames for `seconds`, returns (finals, agent_sent, reply_done,
    text_events, binary_bytes)."""
    finals = 0
    sent = 0
    done = 0
    text_events = []
    binary_bytes = 0
    end = time.time() + seconds
    while time.time() < end:
        try:
            f = ws.recv_frame()
        except socket.timeout:
            break
        if f is None:
            break
        opcode, payload = f
        if opcode == 0x8:  # close
            break
        if opcode == 0x2:
            binary_bytes += len(payload)
            continue
        if opcode != 0x1:
            continue
        try:
            j = json_loads(payload)
        except ValueError:
            continue
        t = j.get("type", "")
        text_events.append(payload.decode(errors="replace"))
        if t == "speech.final":
            finals += 1
        elif t == "agent.sent":
            sent += 1
        elif t == "agent.reply.done":
            done += 1
    return finals, sent, done, text_events, binary_bytes


def json_loads(payload):
    import json

    return json.loads(payload.decode())


def stream_wav(ws, wav_path, chunk_samples=640):
    w = wave.open(wav_path, "rb")
    if w.getframerate() != 16000 or w.getnchannels() != 1 or w.getsampwidth() != 2:
        raise WsError(f"fixture must be 16k mono i16: {wav_path}")
    total = w.getnframes()
    sent = 0
    while sent < total:
        n = min(chunk_samples, total - sent)
        ws.send_binary(w.readframes(n))
        sent += n
        # 640 samples @ 16 kHz = 40 ms: stream at (slightly faster than)
        # real-time pace so the daemon never sees the ring starve or overflow.
        time.sleep(0.03)
    w.close()


# ---------------------------------------------------------------- modes ----

def mode_page(host, port):
    head, body = http_get(host, port, "/")
    status = head.split("\r\n")[0]
    marker = b"persona" in body and b"audio.flush" in body and b"<script>" in body
    if " 200 " not in status or not marker:
        print(f"RESULT page FAIL: {status} marker={marker}")
        return 1
    head404, _ = http_get(host, port, "/nope")
    if " 404 " not in head404.split("\r\n")[0]:
        print(f"RESULT page FAIL: /nope -> {head404.split(chr(13))[0]}")
        return 1
    print("RESULT page OK")
    return 0


def mode_hello(host, port):
    ws = Ws(host, port)
    op, payload = ws.recv_frame()
    if op != 0x1 or b'"type":"hello"' not in payload:
        print("RESULT hello FAIL: no hello event")
        return 1
    # Second connection must be rejected (403 handshake failure) while the
    # first stays up — send a binary frame through it to prove it is alive.
    try:
        Ws(host, port)
        print("RESULT hello FAIL: second connection was ACCEPTED")
        return 1
    except WsError:
        pass
    ws.send_binary(b"\x00\x01\x02\x03")  # audio-ish bytes; server pushes to ring
    ws.close()
    print("RESULT hello OK")
    return 0


def mode_race(host, port):
    """Two SIMULTANEOUS connects: exactly one gets a hello, the other is
    rejected. The loser may be rejected at the HTTP level (403 — validate
    saw active_) or by an immediate going_away close after the upgrade (the
    open_cb one-connection TOCTOU guard) — both count as a rejection. The
    winner must still be up afterwards (send a binary frame through it)."""
    import threading

    def attempt(idx, results):
        try:
            ws = Ws(host, port, timeout=8)
            op, payload = ws.recv_frame()
            if op == 0x8:  # close frame right after the upgrade -> rejected
                results[idx] = ("rejected", None)
            elif op == 0x1 and b'"type":"hello"' in payload:
                results[idx] = ("hello", ws)
            else:
                results[idx] = ("unexpected op=" + str(op), ws)
        except (WsError, socket.timeout) as e:
            # Handshake failed (e.g. 403) -> rejected.
            results[idx] = ("rejected" if isinstance(e, WsError) else "timeout", None)

    results = [None, None]
    threads = []
    for i in range(2):
        t = threading.Thread(target=attempt, args=(i, results))
        t.start()
        threads.append(t)
    for t in threads:
        t.join()

    states = [r[0] for r in results]
    hellos = states.count("hello")
    rejected = states.count("rejected")
    if hellos != 1 or rejected != 1:
        print(f"RESULT race FAIL: hellos={hellos} rejected={rejected} states={states}")
        return 1
    # Winner still up: prove it with a binary frame, then close.
    ws = next(r[1] for r in results if r[0] == "hello")
    ws.send_binary(b"\x00\x01\x02\x03")
    ws.close()
    print("RESULT race OK (one hello, one rejected)")
    return 0


def mode_stream(host, port, wav_path):
    ws = Ws(host, port)
    op, payload = ws.recv_frame()
    if op != 0x1 or b'"type":"hello"' not in payload:
        print("RESULT stream FAIL: no hello")
        return 1
    stream_wav(ws, wav_path)
    finals, sent, done, _, _ = collect_events(ws, 20)
    ws.close()
    print(f"RESULT stream OK finals={finals} agent_sent={sent} reply_done={done}")
    return 0


def mode_audio_out(host, port, wav_path):
    ws = Ws(host, port)
    op, payload = ws.recv_frame()
    if op != 0x1 or b'"type":"hello"' not in payload:
        print("RESULT audio-out FAIL: no hello")
        return 1
    stream_wav(ws, wav_path)
    finals, sent, done, _, binary = collect_events(ws, 45)
    ws.close()
    print(f"RESULT audio-out OK finals={finals} sent={sent} done={done} binary_bytes={binary}")
    return 0


def mode_reconnect(host, port):
    ws = Ws(host, port)
    op, payload = ws.recv_frame()
    if op != 0x1 or b'"type":"hello"' not in payload:
        print("RESULT reconnect FAIL: no hello (1st)")
        return 1
    ws.close()
    time.sleep(1.0)  # let the daemon observe the close
    ws2 = Ws(host, port)
    op2, payload2 = ws2.recv_frame()
    if op2 != 0x1 or b'"type":"hello"' not in payload2:
        print("RESULT reconnect FAIL: no hello (2nd)")
        return 1
    ws2.close()
    print("RESULT reconnect OK")
    return 0


def main():
    if len(sys.argv) < 4:
        print("usage: ws_client.py <mode> <host> <port> [wav]")
        return 2
    mode, host, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
    if mode == "page":
        return mode_page(host, port)
    if mode == "hello":
        return mode_hello(host, port)
    if mode == "race":
        return mode_race(host, port)
    if mode in ("stream", "audio-out"):
        if len(sys.argv) < 5:
            print(f"{mode}: missing wav path")
            return 2
        return (mode_stream if mode == "stream" else mode_audio_out)(host, port, sys.argv[4])
    if mode == "reconnect":
        return mode_reconnect(host, port)
    print(f"unknown mode {mode}")
    return 2


if __name__ == "__main__":
    sys.exit(main())
