#
# MicroPython Web VNC Server (WebSocket-based)
# Copyright (c) 2026 8bitmcu
# License: MIT
#
# Minimum-viable custom remote-display server: NOT RFB/VNC-compatible --
# speaks a small custom binary protocol over a raw WebSocket connection
# to a matching browser client (utils/webvnc.html). Exists because real
# VNC clients (vncd.py) each insist on their own preferred pixel format,
# forcing a conversion pass on every changed row; a client we write
# ourselves can take render_row()'s native RGB565 output as-is and do
# any conversion (to canvas's RGBA) in the browser's JS engine instead,
# far cheaper than doing it on this hardware.
#
# Same background architecture as vncd.py: a machine.Timer tick drives
# an explicit non-blocking state machine (raw sockets, gated by
# select()) instead of a blocking loop, so it runs alongside the shell
# without a thread. At most one client connection at a time.
#
# Wire protocol, server -> client (each one binary WebSocket frame):
#   Init:      0x00, width:u16be, height:u16be, row_height:u16be
#   RowUpdate: 0x01, y:u16be, <row_height * width * 2 bytes RGB565, big-endian>
# Wire protocol, client -> server (one binary WebSocket frame):
#   KeyEvent:  0x02, <utf-8 bytes to inject via env.kvm.inject()>
#
# Usage: `webvncd [port]` to start (default 8080), `webvncd stop` to stop.

import binascii
import hashlib
import struct
import socket
import select
import network
import machine
import micropython
import errno

_MAX_FONT_HEIGHT = 16
# 150ms was a noticeably sluggish ceiling on both input latency (key
# presses) and update-detection latency (how soon a changed row gets
# noticed). 50ms is still well above main.py's 30ms local display timer,
# leaving headroom, but cuts perceived lag roughly 3x.
_TICK_MS = 50

_WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

def get_ip():
    sta = network.WLAN(network.STA_IF)
    if sta.active():
        return sta.ifconfig()[0]
    ap = network.WLAN(network.AP_IF)
    if ap.active():
        return ap.ifconfig()[0]
    return "127.0.0.1"

def _ws_frame_header(opcode, length):
    """Server->client frames are never masked (RFC 6455)."""
    if length <= 125:
        return struct.pack(">BB", 0x80 | opcode, length)
    elif length <= 0xFFFF:
        return struct.pack(">BBH", 0x80 | opcode, 126, length)
    else:
        return struct.pack(">BBQ", 0x80 | opcode, 127, length)

# Connection states. HTTP header reading is delimiter-based (scanned for
# in _process_rx, not a fixed byte count) and handled separately from the
# rest, which -- like vncd.py -- is a reactive "need N bytes" machine.
_ST_HTTP_HEADERS = 0
_ST_WS_HDR = 1      # 2-byte base frame header
_ST_WS_EXTLEN = 2   # 2-byte extended length (only if base len == 126)
_ST_WS_MASK = 3     # 4-byte mask key (client frames are always masked)
_ST_WS_PAYLOAD = 4

def _scan_changed(conn):
    term = conn.env.term
    mv = conn.row565_mv
    found = []
    for idx, y in enumerate(conn.row_indices):
        n565 = term.render_row(y, conn.row565_buf)
        csum = binascii.crc32(mv[:n565])
        if csum != conn.prev_checksums[idx]:
            conn.prev_checksums[idx] = csum
            found.append(y)
    return found

def _queue_row(conn, y):
    env = conn.env
    n565 = env.term.render_row(y, conn.row565_buf)
    y_pos = 0 if y == -1 else env.status_height + y * env.font.HEIGHT

    # Frame header, our own message header, and pixel data queued as
    # three separate outq entries rather than concatenated -- avoids
    # allocating a second full-size copy of the row on top of the one
    # bytes() already has to make (row565_buf gets overwritten by the
    # next row before this one necessarily finishes sending).
    conn.queue(_ws_frame_header(0x2, 3 + n565))
    conn.queue(struct.pack(">BH", 0x01, y_pos))
    conn.queue(bytes(conn.row565_buf[:n565]))

class _Conn:
    def __init__(self, sock, addr, env):
        self.sock = sock
        self.addr = addr
        self.env = env
        self.state = _ST_HTTP_HEADERS
        self.need = 0  # unused in this state -- see _process_rx()
        self.rxbuf = bytearray()
        self.outq = []
        self.outoff = 0
        self.closed = False
        self.pending_close = False

        self.row565_buf = bytearray(env.screen_width * _MAX_FONT_HEIGHT * 2)
        self.row565_mv = memoryview(self.row565_buf)
        self.row_indices = [-1] + list(range(env.rows))  # bar + text rows
        self.prev_checksums = [None] * (1 + env.rows)  # None forces a full first send
        self.handshake_done = False  # scanning for changes starts once this flips

        self._ws_opcode = 0
        self._ws_masked = False
        self._ws_len = 0
        self._ws_mask = b"\x00\x00\x00\x00"

    def queue(self, data):
        self.outq.append(data)

    def close(self):
        if self.closed:
            return
        self.closed = True
        try:
            self.sock.close()
        except OSError:
            pass
        self.env.sts.notify(f"webvncd: {self.addr} disconnected")

def _handle_http_headers(conn, header_bytes):
    key = None
    for line in header_bytes.split(b"\r\n"):
        if line.lower().startswith(b"sec-websocket-key:"):
            key = line.split(b":", 1)[1].strip()
            break

    if key is None:
        conn.pending_close = True
        return

    accept = binascii.b2a_base64(
        hashlib.sha1(key + _WS_GUID).digest()).strip()
    conn.queue(b"HTTP/1.1 101 Switching Protocols\r\n"
              b"Upgrade: websocket\r\n"
              b"Connection: Upgrade\r\n"
              b"Sec-WebSocket-Accept: " + accept + b"\r\n\r\n")

    env = conn.env
    init = struct.pack(">BHHH", 0x00, env.screen_width, env.screen_height,
                       env.font.HEIGHT)
    conn.queue(_ws_frame_header(0x2, len(init)))
    conn.queue(init)

    conn.state, conn.need = _ST_WS_HDR, 2
    conn.handshake_done = True

def _advance(conn, chunk):
    st = conn.state

    if st == _ST_WS_HDR:
        b0, b1 = chunk[0], chunk[1]
        conn._ws_opcode = b0 & 0x0F
        conn._ws_masked = bool(b1 & 0x80)
        plen = b1 & 0x7F
        if plen == 126:
            conn.state, conn.need = _ST_WS_EXTLEN, 2
        elif plen == 127:
            conn.pending_close = True  # not needed for our tiny messages
        else:
            conn._ws_len = plen
            if conn._ws_masked:
                conn.state, conn.need = _ST_WS_MASK, 4
            else:
                conn.state, conn.need = _ST_WS_PAYLOAD, plen

    elif st == _ST_WS_EXTLEN:
        conn._ws_len = struct.unpack(">H", chunk)[0]
        if conn._ws_masked:
            conn.state, conn.need = _ST_WS_MASK, 4
        else:
            conn.state, conn.need = _ST_WS_PAYLOAD, conn._ws_len

    elif st == _ST_WS_MASK:
        conn._ws_mask = chunk
        conn.state, conn.need = _ST_WS_PAYLOAD, conn._ws_len

    elif st == _ST_WS_PAYLOAD:
        if conn._ws_masked:
            mask = conn._ws_mask
            payload = bytearray(len(chunk))
            for i in range(len(chunk)):
                payload[i] = chunk[i] ^ mask[i & 3]
        else:
            payload = chunk

        opcode = conn._ws_opcode
        if opcode in (0x1, 0x2) and payload and payload[0] == 0x02:  # KeyEvent
            try:
                text = bytes(payload[1:]).decode('utf-8')
            except UnicodeError:
                text = None
            if text:
                conn.env.kvm.inject(text)
        elif opcode == 0x8:  # close
            conn.pending_close = True
        elif opcode == 0x9:  # ping -> pong
            conn.queue(_ws_frame_header(0xA, len(payload)))
            conn.queue(bytes(payload))
        # 0xA (pong) and anything else: ignore

        conn.state, conn.need = _ST_WS_HDR, 2

def _process_rx(conn):
    if conn.state == _ST_HTTP_HEADERS:
        idx = conn.rxbuf.find(b"\r\n\r\n")
        if idx < 0:
            return
        header_bytes = bytes(conn.rxbuf[:idx])
        conn.rxbuf = conn.rxbuf[idx + 4:]
        _handle_http_headers(conn, header_bytes)

    while not conn.pending_close and len(conn.rxbuf) >= conn.need:
        chunk = bytes(conn.rxbuf[:conn.need])
        conn.rxbuf = conn.rxbuf[conn.need:]
        _advance(conn, chunk)

def _drain_tx(conn):
    """Sends one queued item's worth of data. Returns True if the caller
    should immediately try again (this item fully sent and more may be
    queued), False if it should stop for this tick (queue empty, or the
    socket buffer is full -- EAGAIN, not an error, just try next tick)."""
    if not conn.outq:
        return False
    item = conn.outq[0]
    try:
        sent = conn.sock.send(memoryview(item)[conn.outoff:])
    except OSError as e:
        if e.args[0] == errno.EAGAIN:
            return False
        conn.pending_close = True
        return False
    conn.outoff += sent
    if conn.outoff >= len(item):
        conn.outq.pop(0)
        conn.outoff = 0
        return True
    # Partial send -- socket buffer is momentarily full, same as EAGAIN.
    return False

class _Server:
    def __init__(self, env, port):
        self.env = env
        self.conn = None
        self._rsocks = []
        self._wsocks = []

        self.listen_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            self.listen_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        except (OSError, AttributeError):
            pass
        self.listen_sock.bind(('0.0.0.0', port))
        self.listen_sock.listen(1)

        # This board appears to only have 4 hardware timers (0-3), all
        # already spoken for: 0/1 are main.py's display/status-bar
        # timers, 2 is used transiently by the audio player, 3 by
        # vncd.py. Reusing vncd.py's id here rather than failing outright
        # -- these are alternative implementations of the same feature,
        # not something meant to run at the same time. Running both
        # servers at once will misbehave (whichever inits second just
        # reconfigures the same hardware timer out from under the
        # first) -- a real fix would need a single shared dispatcher
        # multiplexing one timer across callbacks, not worth building
        # until there's an actual need to run both together.
        self.timer = machine.Timer(3)
        self.timer.init(period=_TICK_MS, mode=machine.Timer.PERIODIC,
                        callback=self._tick_isr)

    def _tick_isr(self, t):
        try:
            micropython.schedule(self._tick, 0)
        except RuntimeError:
            pass

    def _tick(self, _):
        try:
            self._do_tick()
        except Exception:
            pass

    def _do_tick(self):
        conn = self.conn

        rsocks = self._rsocks
        rsocks.clear()
        rsocks.append(self.listen_sock)
        if conn and not conn.closed:
            rsocks.append(conn.sock)

        wsocks = self._wsocks
        wsocks.clear()
        if conn and not conn.closed and conn.outq:
            wsocks.append(conn.sock)

        rready, wready, _ = select.select(rsocks, wsocks, [], 0)

        if self.listen_sock in rready:
            try:
                csock, addr = self.listen_sock.accept()
                if conn is None or conn.closed:
                    # Non-blocking so a slow/stalled client can never make
                    # send() block inside this timer-driven tick -- see
                    # _drain_tx()'s EAGAIN handling below.
                    csock.setblocking(False)
                    self.conn = conn = _Conn(csock, addr[0], self.env)
                    self.env.sts.notify(f"webvncd: {addr[0]} connected")
                else:
                    csock.close()  # already have a client -- reject
            except OSError:
                pass

        if not conn or conn.closed:
            return

        if conn.sock in rready:
            try:
                data = conn.sock.recv(4096)
            except OSError:
                data = None
                conn.pending_close = True
            if data == b"":
                conn.pending_close = True
            elif data:
                conn.rxbuf.extend(data)

        if not conn.pending_close:
            _process_rx(conn)

        # No client-driven request/response cycle here (unlike RFB) --
        # once past the handshake, just check for changes every tick and
        # push whatever's found. No long-poll/wait bookkeeping needed.
        if not conn.pending_close and conn.handshake_done:
            for y in _scan_changed(conn):
                _queue_row(conn, y)

        # Drain as much of the queue as the socket will currently take,
        # rather than one item per tick -- a burst of changed rows (a
        # scroll, a big redraw) can queue dozens of items, and at one
        # item per _TICK_MS that could take seconds to catch up even
        # though the socket itself could clear it in milliseconds.
        # _drain_tx() only returns True after a full item send, so this
        # is bounded by the queue length at loop entry; the explicit cap
        # is just defense in depth against a slow client refilling it as
        # fast as we drain, which would otherwise keep this tick running
        # instead of returning to the scheduler.
        if conn.sock in wready:
            drains_left = 64
            while conn.outq and drains_left > 0:
                drains_left -= 1
                if not _drain_tx(conn):
                    break

        if conn.pending_close and not conn.outq:
            conn.close()

    def stop(self):
        self.timer.deinit()
        if self.conn:
            self.conn.close()
        try:
            self.listen_sock.close()
        except OSError:
            pass

_server = None

def main(env, args):
    global _server

    if args and args[0] == "stop":
        if _server:
            _server.stop()
            _server = None
            print("webvncd stopped")
        else:
            print("webvncd not running")
        return

    if _server:
        print("webvncd already running\nuse 'webvncd stop' to stop it")
        return

    port = int(args[0]) if args else 8080
    _server = _Server(env, port)
    print(f"webvncd started on {get_ip()}:{port}\nopen utils/webvnc.html and "
         f"connect to that address\n'webvncd stop' to stop")
