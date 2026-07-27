#
# MicroPython TUI IRC Client
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import socket
import select
import random
import sys
import time
import errno

# Minimum time between chat_win/env.tui redraws. A burst of several IRC
# lines can arrive in one recv() (registration notices, a big NAMES reply,
# etc.) -- drawing on every single line fires off back-to-back SPI writes
# with no gap between them, which was found to trip a WDT_RESET. Throttling
# to this interval keeps redraws from piling up during a burst.
_DRAW_INTERVAL_MS = 50

def _parse(line):
    """Parse a raw IRC line.
    Returns (prefix, command, params, trailing)
      prefix   - 'nick!user@host' or server name, or None
      command  - e.g. 'PRIVMSG', 'JOIN', '001'
      params   - list of middle params
      trailing - string after ' :' or None
    """
    prefix = None
    if line.startswith(':'):
        prefix, line = line[1:].split(' ', 1)
    parts = line.split(' :', 1)
    middle = parts[0].split()
    trailing = parts[1] if len(parts) > 1 else None
    command = middle[0] if middle else ''
    params = middle[1:]
    return prefix, command, params, trailing

def get_default_nick():
    """Generates a random nickname in the format vtOS_XXXX."""
    chars = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789'
    rand = ''.join(random.choice(chars) for _ in range(4))
    return f"vtOS_{rand}"

def nick_from_prefix(prefix):
    """Extract nick from 'nick!user@host'."""
    if prefix and '!' in prefix:
        return prefix.split('!', 1)[0]
    return prefix

def format_msg(msg):
    """Format a parsed IRC tuple into a human-readable string (no trailing newline)."""
    if msg is None:
        return ""
    prefix, command, params, trailing = msg
    nick = nick_from_prefix(prefix)
    if command == 'PRIVMSG':
        return f"\x1b[1m{nick}\x1b[22m: {trailing or ''}"
    if command == 'JOIN':
        return f"* {nick} joined {trailing or (params[0] if params else '')}"
    if command == 'PART':
        chan = params[0] if params else ''
        reason = f" ({trailing})" if trailing else ''
        return f"* {nick} left {chan}{reason}"
    if command == 'QUIT':
        return f"* {nick} quit ({trailing or ''})"
    if command == 'NICK':
        return f"* {nick} is now known as {trailing or (params[0] if params else '')}"
    if command == 'NOTICE':
        return f"-{nick or 'server'}- {trailing or ''}"
    if command.isdigit():
        # Numeric reply — show the trailing text (MOTD, errors, etc.)
        return trailing or ' '.join(params[1:])
    return f"[{command}] {trailing or ' '.join(params)}"


class IRCClient:
    def __init__(self, host, port=6667):
        self.host = host
        self.port = port
        self.sock = None
        self.nick = None
        self._buf = b''

    def connect(self, nick, user=None, realname=None, password=None):
        """Open TCP connection and register with the server."""
        self.nick = nick
        user = user or nick
        realname = realname or nick

        addr = socket.getaddrinfo(self.host, self.port)[0][-1]
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

        # Non-blocking + select-polled connect, for the same reason
        # _recv_line() polls instead of blocking: a raw blocking connect()
        # can sit in one uninterrupted C call for as long as the TCP
        # handshake takes, starving the fast_loop timer's
        # micropython.schedule() queue the whole time.
        self.sock.setblocking(False)
        try:
            self.sock.connect(addr)
        except OSError as e:
            if e.args[0] not in (errno.EAGAIN, errno.EINPROGRESS):
                raise
        deadline = time.ticks_add(time.ticks_ms(), 10000)
        while True:
            _, w, _ = select.select([], [self.sock], [], 0.1)
            if w:
                break
            if time.ticks_diff(deadline, time.ticks_ms()) <= 0:
                self.sock.close()
                self.sock = None
                raise OSError("connect timed out")
        self.sock.setblocking(True)
        try:
            err = self.sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
        except AttributeError:
            err = 0  # getsockopt not implemented on this port -- skip the check
        if err != 0:
            self.sock.close()
            self.sock = None
            raise OSError(err, "connect failed")

        if password:
            self.raw(f'PASS {password}')
        self.raw(f'NICK {nick}')
        self.raw(f'USER {user} 0 * :{realname}')

    def disconnect(self, reason='bye'):
        if self.sock:
            try:
                self.raw(f'QUIT :{reason}')
            except:
                pass
            self.sock.close()
            self.sock = None

    def join(self, channel):
        self.raw(f'JOIN {channel}')

    def part(self, channel, reason=''):
        self.raw(f'PART {channel} :{reason}' if reason else f'PART {channel}')

    def send(self, target, text):
        """Send a PRIVMSG."""
        self.raw(f'PRIVMSG {target} :{text}')

    def raw(self, line):
        """Send a raw IRC line (without \\r\\n)."""
        self.sock.send((line + '\r\n').encode())

    def poll(self, timeout=0):
        """Non-blocking read. Returns a parsed message tuple or None.
        Handles PING automatically (same as read())."""
        r, _, _ = select.select([self.sock], [], [], timeout)
        if not r:
            return None
        return self.read()

    def read(self):
        """Blocking read of one IRC line. Handles PING automatically.
        Returns (prefix, command, params, trailing) or None on disconnect."""
        while True:
            msg = self._recv_line()
            if msg is None:
                return None
            _, command, _, trailing = msg
            if command == 'PING':
                self.raw(f'PONG :{trailing}')
                continue
            return msg

    def _recv_line(self):
        """Read bytes until \\r\\n, buffering partial data.

        Polls with a short select() timeout rather than calling recv()
        directly -- the server can go silent for several seconds during
        registration (ident/rDNS lookup), and a single uninterrupted
        blocking recv() keeps the interpreter from ever returning to the
        bytecode loop for that whole stretch, starving anything driven by
        micropython.schedule() (e.g. the statusbar's Timer callback) until
        its schedule queue backs up."""
        while b'\r\n' not in self._buf:
            r, _, _ = select.select([self.sock], [], [], 0.1)
            if not r:
                continue
            chunk = self.sock.recv(512)
            if not chunk:
                return None
            self._buf += chunk
        line, self._buf = self._buf.split(b'\r\n', 1)
        return _parse(line.decode('utf-8', 'ignore'))

def main(env, args):
    host    = args[0] if len(args) > 0 else None
    port    = int(args[1]) if len(args) > 1 else 6667
    nick    = args[2] if len(args) > 2 else get_default_nick()
    channel = args[3] if len(args) > 3 else None

    if not host:
        print("Usage: irc <host> [port] [nick] [#channel]")
        return

    CLR  = "\x1b[0m"
    RED  = "\x1b[38;5;210m"
    CYAN = "\x1b[38;5;45m"

    env.tui.enter_altscreen()
    env.tui.cursor_hide()

    chat_win = env.tui.make_block("", 0, 0,
          width=env.cols, height=env.rows-1,
          fg=252, bg=18,
          scroll=True, wrap=True)

    chat_input = env.tui.make_input("",
        0, env.rows-1,
        width=env.cols,
        fg=252, bg=0, input_bg=0,
        decorations=False)

    client = IRCClient(host, port)
    client.connect(nick)

    last_draw = time.ticks_ms()
    while True:
        msg = client.read()
        chat_win.push(format_msg(msg) + "\n")
        done = msg and msg[1] in ('376', '422')
        now = time.ticks_ms()
        if done or time.ticks_diff(now, last_draw) >= _DRAW_INTERVAL_MS:
            chat_win.draw()
            env.tui.draw()
            last_draw = now
        if done:
            break

    if channel:
        client.join(channel)

    env.tui.cursor_show()

    last_draw = time.ticks_ms()
    while True:
        now = time.ticks_ms()
        if time.ticks_diff(now, last_draw) >= _DRAW_INTERVAL_MS:
            chat_win.draw()
            chat_input.draw()
            env.tui.draw()
            last_draw = now

        # Polled separately rather than combined into one
        # select([client.sock, sys.stdin], ...) call -- a real socket and
        # a custom dupterm-backed stream object together in a single
        # select() is an unusual combination (most code selects on one or
        # the other, not both), and the registration loop's socket-only
        # select() has been solid while this combined form hasn't.
        sock_ready, _, _ = select.select([client.sock], [], [], 0)
        stdin_ready, _, _ = select.select([sys.stdin], [], [], 0)
        if not sock_ready and not stdin_ready:
            time.sleep_ms(100)

        if sock_ready:
            msg = client.poll()
            if msg:
                if msg[1] == 'PRIVMSG':
                    chat_win.push(CYAN + format_msg(msg) + CLR + "\n")
                else:
                    chat_win.push(format_msg(msg) + "\n")

        if stdin_ready:
            try:
                char = sys.stdin.read(1)
            except UnicodeError:
                char = None
            if char is None:
                pass
            elif char in ('\r', '\n'):
                text = chat_input.value
                if not text:
                    pass

                # Handle internal commands
                elif text.startswith("/nick "):
                    new_nick = text.split(" ", 1)[1]
                    client.raw(f"NICK {new_nick}")
                    nick = new_nick # Update local variable for UI formatting
                    chat_win.push(f"--- Nick changed to {nick} ---\n")
                    chat_input.set("")
                elif text.startswith("/join "):
                    target_channel = text.split(" ", 1)[1]
                    client.join(target_channel)
                    channel = target_channel # Update local channel for sending
                    chat_win.push(f"--- Joined {channel} ---\n")
                    chat_input.set("")
                elif text == "/quit":
                    client.disconnect()
                    env.tui.exit_altscreen()
                    env.tui.cursor_show()
                    return

                # Handle normal chat
                elif channel:
                    client.send(channel, text)
                    chat_win.push(RED + f"\x1b[1m{nick}\x1b[22m: {text}" + CLR + "\n")
                    chat_input.set("")
                else:
                    chat_win.push("--- No channel joined ---\n")
                    chat_input.set("")

            elif char in ('\x08', '\x7f'):
                chat_input.backspace()
            elif char == '\x1b':
                # TODO: does not exit cleanly
                client.disconnect()
                env.tui.exit_altscreen()
                env.tui.cursor_show()
                return
            else:
                chat_input.push(char)
