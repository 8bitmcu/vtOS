#
# MicroPython Telnet Server
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import socket
import select
import network
import machine
import micropython

IAC  = 255
DONT = 254
DO   = 253
WONT = 252
WILL = 251
SB   = 250
SE   = 240

OPT_ECHO = 1
OPT_SGA  = 3
OPT_NAWS = 31

_TICK_MS = 150
_AUTH_TIMEOUT_TICKS = 200  # ~30s at _TICK_MS

_ST_AUTH_USER = 0
_ST_AUTH_PASS = 1
_ST_SHELL     = 2


def get_ip():
    sta = network.WLAN(network.STA_IF)
    if sta.active():
        return sta.ifconfig()[0]
    ap = network.WLAN(network.AP_IF)
    if ap.active():
        return ap.ifconfig()[0]
    return "127.0.0.1"


class _Conn:
    def __init__(self, sock, addr, auth_user, auth_pass):
        self.sock = sock
        self.addr = addr
        self.auth_user = auth_user
        self.auth_pass = auth_pass

        self.state = _ST_AUTH_USER
        self.closed = False          # set only by close() itself
        self.pending_close = False   # set by _do_tick to request a close
        self.authed = False
        self.line = bytearray()
        self.username = None
        self.wait_ticks = 0

        # Telnet IAC state machine -- mirrors telnet.py's client-side one
        # (_process_complex_data/_handle_negotiation), answering as the
        # server instead.
        self.command_mode = False
        self.current_option_cmd = None
        self.in_subneg = False
        self.subneg_buf = bytearray()

        # We do our own echoing from here on (per character during auth,
        # via the shared shell's own echo once bridged) -- tell the client
        # not to locally echo, and that we don't need Go-Ahead signaling.
        # DO NAWS is unilateral too; we don't act on the window size, but
        # asking is harmless and keeps well-behaved clients happy.
        sock.send(bytes([IAC, WILL, OPT_ECHO, IAC, WILL, OPT_SGA, IAC, DO, OPT_NAWS]))
        sock.send(b"Username: ")

    def close(self, env):
        if self.closed:
            return
        self.closed = True
        if self.authed:
            env.kvm.set_mirror(None)
        try:
            self.sock.close()
        except OSError:
            pass

    def _strip_telnet(self, data):
        """Runs data through the IAC state machine, answering negotiation
        in place and returning only the "real" data bytes."""
        clean = bytearray()
        i = 0
        while i < len(data):
            byte = data[i]

            if not self.command_mode and byte != IAC:
                clean.append(byte)
                i += 1
            elif not self.command_mode and byte == IAC:
                self.command_mode = True
                i += 1
            elif byte == SB:
                self.in_subneg = True
                self.subneg_buf = bytearray()
                i += 1
            elif self.in_subneg:
                if byte == SE:
                    self.in_subneg = False
                    self.command_mode = False
                elif byte != IAC:
                    self.subneg_buf.append(byte)
                i += 1
            elif byte in (WILL, WONT, DO, DONT):
                self.current_option_cmd = byte
                i += 1
            elif self.current_option_cmd:
                self._negotiate(self.current_option_cmd, byte)
                self.current_option_cmd = None
                self.command_mode = False
                i += 1
            else:
                # NOP or another single-byte command we don't act on
                self.command_mode = False
                i += 1

        return bytes(clean)

    def _negotiate(self, command, option):
        if option in (OPT_ECHO, OPT_SGA, OPT_NAWS):
            # Exactly what we already unilaterally offered/requested at
            # connect -- any DO/WILL/WONT/DONT the client sends back about
            # them is just acknowledgement, nothing further to send (and
            # definitely don't blanket-refuse our own options below).
            return
        # Anything else the client proposes, refuse. Never reply to a
        # WONT/DONT -- that's how negotiation loops happen.
        if command == DO:
            self.sock.send(bytes([IAC, WONT, option]))
        elif command == WILL:
            self.sock.send(bytes([IAC, DONT, option]))

    def feed(self, data, env):
        clean = self._strip_telnet(data)
        if not clean:
            return

        if self.state == _ST_SHELL:
            # CRLF -> CR: the shell only expects a single '\r'/'\n' to
            # submit a line: a raw trailing '\n' after '\r' would submit
            # an extra blank line and cause a harmless but noisy
            # double-prompt.
            clean = clean.replace(b"\r\n", b"\r")
            # inject()'s ghost-key ring buffer is only 32 bytes (see
            # tdeck_kvm.c) and silently drops anything past that -- fine
            # for normal keystroke-at-a-time typing, but a large pasted
            # chunk in one recv() can lose its tail. Same constraint every
            # other inject() caller (vncd's key events, boot.py's
            # trackball injection) already lives with.
            env.kvm.inject(str(clean, "utf-8"))
            return

        # Pre-auth: line-buffer username/password. Echo what's typed for
        # the username (visible feedback); don't echo the password at all.
        for b in clean:
            if b in (13, 10):  # \r or \n
                self._submit_line(env)
            elif b in (8, 127):  # backspace/DEL
                if self.line:
                    self.line = self.line[:-1]
                    if self.state == _ST_AUTH_USER:
                        self.sock.send(b"\b \b")
            elif 32 <= b <= 126:
                self.line.append(b)
                if self.state == _ST_AUTH_USER:
                    self.sock.send(bytes([b]))

    def _submit_line(self, env):
        text = str(bytes(self.line), "utf-8")
        self.line = bytearray()

        if self.state == _ST_AUTH_USER:
            self.username = text
            self.sock.send(b"\r\nPassword: ")
            self.state = _ST_AUTH_PASS
            return

        # _ST_AUTH_PASS
        if self.username == self.auth_user and text == self.auth_pass:
            self.sock.send(b"\r\n")
            self.authed = True
            self.state = _ST_SHELL
            env.kvm.set_mirror(self.sock)
        else:
            self.sock.send(b"\r\nLogin incorrect\r\nUsername: ")
            self.username = None
            self.state = _ST_AUTH_USER


class _Server:
    def __init__(self, env, auth_user, auth_pass, port):
        self.env = env
        self.auth_user = auth_user
        self.auth_pass = auth_pass
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
        self.listen_sock.setblocking(False)

        # See vncd.py: timer 3 is the one hardware timer this board has
        # spare for a persistent background app (0/1/2 are already spoken
        # for elsewhere).
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

        rready, _, _ = select.select(rsocks, [], [], 0)

        if self.listen_sock in rready:
            try:
                csock, addr = self.listen_sock.accept()
                if conn is None or conn.closed:
                    csock.setblocking(False)
                    self.conn = conn = _Conn(csock, addr[0], self.auth_user, self.auth_pass)
                    self.env.sts.notify(f"telnetd: {addr[0]} connected")
                else:
                    csock.close()  # already have a client -- reject
            except OSError:
                pass

        if not conn or conn.closed:
            return

        if conn.sock in rready:
            try:
                data = conn.sock.recv(1024)
            except OSError:
                data = None
                conn.pending_close = True
            if data == b"":
                conn.pending_close = True
            elif data:
                conn.wait_ticks = 0
                conn.feed(data, self.env)

        if not conn.pending_close and not conn.authed:
            conn.wait_ticks += 1
            if conn.wait_ticks >= _AUTH_TIMEOUT_TICKS:
                conn.pending_close = True

        if conn.pending_close:
            addr = conn.addr
            conn.close(self.env)
            self.env.sts.notify(f"telnetd: {addr} disconnected")

    def stop(self):
        self.timer.deinit()
        if self.conn:
            self.conn.close(self.env)
        try:
            self.listen_sock.close()
        except OSError:
            pass


_server = None  # module-level -- persists across repeated `telnetd` calls

def main(env, args):
    global _server

    if args and args[0] == "stop":
        if _server:
            _server.stop()
            _server = None
            print("telnetd stopping...")
        else:
            print("telnetd not running")
        return

    auth_user = args[0] if len(args) > 0 else None
    auth_pass = args[1] if len(args) > 1 else None
    port = int(args[2]) if len(args) > 2 else 23

    if auth_user is None or auth_pass is None:
        print("Usage: telnetd <user> <password> [port]\n       telnetd stop")
        return

    if _server is not None:
        print("telnetd: already running\nuse 'telnetd stop' to stop it")
        return

    _server = _Server(env, auth_user, auth_pass, port)
    print(f"telnetd: listening on {get_ip()}:{port}")
