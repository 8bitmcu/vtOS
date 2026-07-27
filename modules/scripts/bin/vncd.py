#
# MicroPython VNC Server
# Copyright (c) 2026 8bitmcu
# License: MIT
#
# Minimal RFB 3.8 server exposing the terminal screen over VNC. Pixels
# come from vt.VT.render_row() (fb.c's render_row_rgb565()), rendered on
# demand per row -- no persistent framebuffer.

import binascii
import struct
import socket
import select
import network
import machine
import micropython

_MAX_FONT_HEIGHT = 16
# Every tick makes one micropython.schedule() attempt, which competes for
# the same limited schedule queue as main.py's own timers (draw/status
# bar) -- neither side checks whether its schedule() call actually got
# queued, so under contention one silently loses its turn. 50ms (20/sec)
# was frequent enough to occasionally bump the status bar's once-a-second
# refresh; 150ms cuts that 3x while staying responsive for keyboard input.
_TICK_MS = 150
_MAX_WAIT_TICKS = 10  # 1.5s at _TICK_MS

_PIXEL_FORMAT = struct.pack(">BBBBHHHBBB",
                            32, 24,        # bits-per-pixel, depth
                            0, 1,          # big-endian-flag, true-color-flag
                            255, 255, 255, # red-max, green-max, blue-max
                            16, 8, 0) + b"\x00\x00\x00"  # red/green/blue-shift + padding
assert len(_PIXEL_FORMAT) == 16

_DEFAULT_FORMAT = (32, 0, 255, 255, 255, 16, 8, 0)

_SERVER_NAME = b"vtOS"

_KEYSYM_MAP = {
    0xFF08: "\x08",    # BackSpace
    0xFF09: "\t",       # Tab
    0xFF0D: "\r",       # Return
    0xFF1B: "\x1b",     # Escape
    0xFF51: "\x1b[D",   # Left
    0xFF52: "\x1b[A",   # Up
    0xFF53: "\x1b[C",   # Right
    0xFF54: "\x1b[B",   # Down
}

# Connection states -- each maps to how many bytes are needed before
# _advance() can process the next step.
_ST_VERSION = 0
_ST_SECTYPE = 1
_ST_CLIENTINIT = 2
_ST_MSGTYPE = 3
_ST_SETPIXFMT = 4
_ST_SETENC_HDR = 5
_ST_SETENC_LIST = 6
_ST_FBUR = 7
_ST_KEYEVENT = 8
_ST_POINTER = 9
_ST_CUTTEXT_HDR = 10
_ST_CUTTEXT_BODY = 11

def get_ip():
    sta = network.WLAN(network.STA_IF)
    if sta.active():
        return sta.ifconfig()[0]
    ap = network.WLAN(network.AP_IF)
    if ap.active():
        return ap.ifconfig()[0]
    return "127.0.0.1"

def _inject_key(env, keysym):
    if 0x20 <= keysym <= 0x7e:
        env.kvm.inject(chr(keysym))
    else:
        seq = _KEYSYM_MAP.get(keysym)
        if seq:
            env.kvm.inject(seq)

def _parse_pixel_format(body):
    (bpp, _depth, big_endian, true_color, r_max, g_max, b_max,
     r_shift, g_shift, b_shift) = struct.unpack(">BBBBHHHBBB", body[:13])
    return (bpp, big_endian, true_color, r_max, g_max, b_max,
           r_shift, g_shift, b_shift)

def _rgb565_row_to_pixfmt(src, n_bytes, dst, fmt):
    """Converts n_bytes of big-endian RGB565 (render_row()'s native
    output) into the true-color format `fmt` describes -- (bpp,
    big_endian, red_max, green_max, blue_max, red_shift, green_shift,
    blue_shift). Returns the number of bytes written to dst.

    5/6-bit source channels are expanded to 8 bits by bit replication,
    then scaled to the target's own max-value range before shifting into
    place.
    """
    bpp, big_endian, r_max, g_max, b_max, r_shift, g_shift, b_shift = fmt
    nbytes = bpp // 8
    oi = 0
    for i in range(0, n_bytes, 2):
        v = (src[i] << 8) | src[i + 1]
        r5 = (v >> 11) & 0x1F
        g6 = (v >> 5) & 0x3F
        b5 = v & 0x1F
        r8 = (r5 << 3) | (r5 >> 2)
        g8 = (g6 << 2) | (g6 >> 4)
        b8 = (b5 << 3) | (b5 >> 2)
        val = (((r8 * r_max) // 255) << r_shift |
              ((g8 * g_max) // 255) << g_shift |
              ((b8 * b_max) // 255) << b_shift)
        if big_endian:
            for k in range(nbytes):
                dst[oi + k] = (val >> ((nbytes - 1 - k) * 8)) & 0xFF
        else:
            for k in range(nbytes):
                dst[oi + k] = (val >> (k * 8)) & 0xFF
        oi += nbytes
    return oi

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

def _queue_update(conn, changed):
    env = conn.env
    term = env.term
    width = env.screen_width
    f_height = env.font.HEIGHT
    top_offset = env.status_height
    fmt = conn.current_format

    conn.queue(struct.pack(">BBH", 0, 0, len(changed)))
    for y in changed:
        n565 = term.render_row(y, conn.row565_buf)
        n = _rgb565_row_to_pixfmt(conn.row565_buf, n565, conn.row_buf, fmt)
        y_pos = 0 if y == -1 else top_offset + y * f_height
        # Header and payload queued separately -- concatenating them would
        # allocate a second full-size copy of the row on top of the one
        # bytes() already has to make (conn.row_buf gets overwritten by
        # the next row before this one necessarily finishes sending).
        conn.queue(struct.pack(">HHHHI", 0, y_pos, width, f_height, 0))
        conn.queue(bytes(conn.row_buf[:n]))

class _Conn:
    def __init__(self, sock, addr, env):
        self.sock = sock
        self.addr = addr
        self.env = env
        self.state = _ST_VERSION
        self.need = 12
        self.rxbuf = bytearray()
        self.outq = []
        self.outoff = 0
        self.closed = False
        self.pending_close = False

        self.row565_buf = bytearray(env.screen_width * _MAX_FONT_HEIGHT * 2)
        self.row565_mv = memoryview(self.row565_buf)  # reused for crc32 -- avoids a new memoryview per row per scan
        self.row_buf = bytearray(env.screen_width * _MAX_FONT_HEIGHT * 4)
        self.current_format = _DEFAULT_FORMAT
        self.row_indices = [-1] + list(range(env.rows))  # bar + text rows, computed once
        self.prev_checksums = [None] * (1 + env.rows)  # None forces a full first send

        self.awaiting_update = False
        self.wait_ticks = 0
        self._setenc_count = 0

        self.queue(b"RFB 003.008\n")  # server speaks first, unprompted

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
        self.env.sts.notify(f"vncd: {self.addr} disconnected")

def _advance(conn, chunk):
    st = conn.state

    if st == _ST_VERSION:
        conn.queue(b"\x01\x01")  # offer only security type 1 (None)
        conn.state, conn.need = _ST_SECTYPE, 1

    elif st == _ST_SECTYPE:
        if chunk != b"\x01":
            reason = b"Only security type 1 (None) is supported"
            conn.queue(struct.pack(">I", 1) +
                      struct.pack(">I", len(reason)) + reason)
            conn.pending_close = True
            return
        conn.queue(struct.pack(">I", 0))  # SecurityResult: OK
        conn.state, conn.need = _ST_CLIENTINIT, 1

    elif st == _ST_CLIENTINIT:
        env = conn.env
        conn.queue(struct.pack(">HH", env.screen_width, env.screen_height) +
                  _PIXEL_FORMAT +
                  struct.pack(">I", len(_SERVER_NAME)) + _SERVER_NAME)
        conn.state, conn.need = _ST_MSGTYPE, 1

    elif st == _ST_MSGTYPE:
        mtype = chunk[0]
        if mtype == 0:      # SetPixelFormat
            conn.state, conn.need = _ST_SETPIXFMT, 3 + 16
        elif mtype == 2:    # SetEncodings
            conn.state, conn.need = _ST_SETENC_HDR, 3
        elif mtype == 3:    # FramebufferUpdateRequest
            conn.state, conn.need = _ST_FBUR, 9
        elif mtype == 4:    # KeyEvent
            conn.state, conn.need = _ST_KEYEVENT, 7
        elif mtype == 5:    # PointerEvent
            conn.state, conn.need = _ST_POINTER, 5
        elif mtype == 6:    # ClientCutText
            conn.state, conn.need = _ST_CUTTEXT_HDR, 7
        else:
            conn.pending_close = True  # unknown type -- can't trust framing

    elif st == _ST_SETPIXFMT:
        (bpp, big_endian, true_color, r_max, g_max, b_max,
         r_shift, g_shift, b_shift) = _parse_pixel_format(chunk[3:19])
        if true_color and bpp in (8, 16, 24, 32):
            conn.current_format = (bpp, big_endian, r_max, g_max, b_max,
                                   r_shift, g_shift, b_shift)
        # else: colour-map/palette mode or an unsupported bpp -- not
        # implemented, keep using whatever format was active.
        conn.state, conn.need = _ST_MSGTYPE, 1

    elif st == _ST_SETENC_HDR:
        count = struct.unpack(">H", chunk[1:3])[0]
        if count:
            conn._setenc_count = count
            conn.state, conn.need = _ST_SETENC_LIST, 4 * count
        else:
            conn.state, conn.need = _ST_MSGTYPE, 1

    elif st == _ST_SETENC_LIST:  # Raw is mandatory anyway; list is ignored
        conn.state, conn.need = _ST_MSGTYPE, 1

    elif st == _ST_FBUR:
        conn.awaiting_update = True
        conn.wait_ticks = 0
        conn.state, conn.need = _ST_MSGTYPE, 1

    elif st == _ST_KEYEVENT:
        if chunk[0]:  # down-flag
            _inject_key(conn.env, struct.unpack(">I", chunk[3:7])[0])
        conn.state, conn.need = _ST_MSGTYPE, 1

    elif st == _ST_POINTER:
        conn.state, conn.need = _ST_MSGTYPE, 1

    elif st == _ST_CUTTEXT_HDR:
        length = struct.unpack(">I", chunk[3:7])[0]
        if length:
            conn.state, conn.need = _ST_CUTTEXT_BODY, length
        else:
            conn.state, conn.need = _ST_MSGTYPE, 1

    elif st == _ST_CUTTEXT_BODY:
        conn.state, conn.need = _ST_MSGTYPE, 1

def _process_rx(conn):
    while not conn.pending_close and len(conn.rxbuf) >= conn.need:
        chunk = bytes(conn.rxbuf[:conn.need])
        conn.rxbuf = conn.rxbuf[conn.need:]  # bytearray slicing, not del -- MicroPython's bytearray doesn't support slice deletion
        _advance(conn, chunk)

def _drain_tx(conn):
    item = conn.outq[0]
    try:
        sent = conn.sock.send(memoryview(item)[conn.outoff:])
    except OSError:
        conn.pending_close = True
        return
    conn.outoff += sent
    if conn.outoff >= len(item):
        conn.outq.pop(0)
        conn.outoff = 0

class _Server:
    def __init__(self, env, port):
        self.env = env
        self.conn = None
        self._rsocks = []  # reused every tick instead of rebuilt -- see _do_tick()
        self._wsocks = []

        self.listen_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            self.listen_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        except (OSError, AttributeError):
            pass
        self.listen_sock.bind(('0.0.0.0', port))
        self.listen_sock.listen(1)

        # This port's machine.Timer only accepts real hardware timer ids
        # (no -1/virtual-timer fallback like some other ports) and this
        # board appears to only have 4 (0-3) -- 0/1 are main.py's display/
        # status-bar timers, 2 is used transiently by the audio player, so
        # 3 is what's left for a persistent app-level background timer.
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
                    self.conn = conn = _Conn(csock, addr[0], self.env)
                    self.env.sts.notify(f"vncd: {addr[0]} connected")
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

        if not conn.pending_close and conn.awaiting_update:
            changed = _scan_changed(conn)
            if changed:
                _queue_update(conn, changed)
                conn.awaiting_update = False
            else:
                conn.wait_ticks += 1
                if conn.wait_ticks >= _MAX_WAIT_TICKS:
                    conn.awaiting_update = False

        if conn.sock in wready and conn.outq:
            _drain_tx(conn)

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

_server = None  # module-level -- persists across repeated `vncd` calls

def main(env, args):
    global _server

    if args and args[0] == "stop":
        if _server:
            _server.stop()
            _server = None
            print("VNC server stopped")
        else:
            print("VNC server not running")
        return

    if _server:
        print("VNC server already running\nuse 'vncd stop' to stop it")
        return

    port = int(args[0]) if args else 5900
    _server = _Server(env, port)
    print(f"VNC server started on {get_ip()}:{port}\n(background); use 'vncd stop' to stop")
