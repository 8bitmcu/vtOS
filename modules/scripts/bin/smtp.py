#
# MicroPython TUI SMTP Email Client
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import gc
import os
import sys
import time
import socket
import binascii
import bin.pop3

FG = 252
BG = 18

CLR = "\x1b[0m"
ERR_FG = "\x1b[38;5;210m"

_COMPOSE_PATH = "/flash/.compose.mail"


class SMTPError(Exception):
    pass


class SMTPClient:
    """ Minimal RFC5321 SMTP client with AUTH LOGIN. TLS mode is inferred
    from the port: 465 is implicit TLS, 25 is plaintext (local/open relay
    use), anything else (587 by default) is STARTTLS. """

    def __init__(self, host, port=587):
        self.host = host
        self.port = port
        self.implicit_tls = (port == 465)
        self.use_starttls = port not in (25, 465)
        self.sock = None

    def _wrap_tls(self):
        import tls

        # No cert store to validate against on this device, same tradeoff
        # gemini.py/requests.py/pop3.py already make for TLS.
        context = tls.SSLContext(tls.PROTOCOL_TLS_CLIENT)
        context.verify_mode = tls.CERT_NONE
        self.sock = context.wrap_socket(self.sock, server_hostname=self.host)

    def connect(self, user, password, timeout=15):
        ai = socket.getaddrinfo(self.host, self.port, 0, socket.SOCK_STREAM)[0]
        s = socket.socket(ai[0], socket.SOCK_STREAM, ai[2])

        if timeout is not None:
            s.settimeout(timeout)

        try:
            s.connect(ai[-1])
            self.sock = s

            if self.implicit_tls:
                self._wrap_tls()

            self._expect(220)  # unsolicited greeting
            self._send_cmd("EHLO vtos", 250)

            if self.use_starttls:
                self._send_cmd("STARTTLS", 220)
                self._wrap_tls()
                # Capabilities must be renegotiated over the encrypted
                # channel per RFC 3207 -- the pre-TLS EHLO doesn't count.
                self._send_cmd("EHLO vtos", 250)

            self._send_cmd("AUTH LOGIN", 334)
            user_b64 = str(binascii.b2a_base64(user.encode('utf-8'))[:-1], 'ascii')
            self._send_cmd(user_b64, 334)
            pass_b64 = str(binascii.b2a_base64(password.encode('utf-8'))[:-1], 'ascii')
            self._send_cmd(pass_b64, 235)
        except (OSError, SMTPError):
            self.close()
            raise

    def _read_reply(self):
        """ Reads one SMTP reply -- possibly several "CCC-text" continuation
        lines terminated by a final "CCC text" line (RFC 5321 4.2), unlike
        POP3's dot-terminated blocks. Returns (code, [lines]). """
        lines = []
        while True:
            line = self.sock.readline()
            if not line:
                raise SMTPError("connection closed by server")
            line = line.decode('utf-8', 'ignore').rstrip('\r\n')
            if len(line) < 4:
                raise SMTPError("malformed reply: %r" % line)
            code = int(line[:3])
            lines.append(line[4:])
            if line[3] != '-':
                return code, lines

    def _expect(self, expect):
        code, lines = self._read_reply()
        if isinstance(expect, int):
            expect = (expect,)
        if code not in expect:
            raise SMTPError("%d %s" % (code, "; ".join(lines)))
        return code, lines

    def _send_cmd(self, cmd, expect):
        self.sock.write(cmd + "\r\n")
        return self._expect(expect)

    def send(self, from_addr, to_addr, data):
        """ Sends one message: MAIL FROM/RCPT TO/DATA. `data` is a full
        RFC5322 message (headers + body) as bytes, e.g. from
        build_message(). """
        self._send_cmd("MAIL FROM:<%s>" % from_addr, 250)
        self._send_cmd("RCPT TO:<%s>" % to_addr, 250)
        self._send_cmd("DATA", 354)
        self.sock.write(_stuff_dot(data))
        self.sock.write(b"\r\n.\r\n")
        self._expect(250)

    def quit(self):
        try:
            self._send_cmd("QUIT", 221)
        finally:
            self.close()

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
        return False


# ─── message building (pure functions, no I/O) ──────────────────────────

def _stuff_dot(data):
    """ RFC 5321 4.5.2 dot-stuffing: a "." that starts a line is doubled,
    so the server can find the real end-of-DATA marker ("\\r\\n.\\r\\n")
    unambiguously. Inverse of pop3.py's _read_multiline() byte-
    unstuffing. """
    if data.startswith(b"."):
        data = b"." + data
    return data.replace(b"\r\n.", b"\r\n..")


_WEEKDAYS = ("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")
_MONTHS = ("", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")


def _format_date():
    """ RFC 5322 3.3 Date header, e.g. "Mon, 27 Jul 2026 10:00:00 +0000".
    MicroPython has no email.utils.formatdate, hence hand-rolled -- and
    it's only as accurate as the device's clock (no NTP sync exists in
    this codebase yet, so this is a known limitation, not addressed
    here). """
    # Indexed rather than unpacked: MicroPython's gmtime() returns an
    # 8-tuple, CPython's a 9-field struct_time -- indexing 0-6 works on
    # both instead of an unpack count mismatch.
    t = time.gmtime()
    year, month, day, hour, minute, second, weekday = t[0], t[1], t[2], t[3], t[4], t[5], t[6]
    return "%s, %02d %s %04d %02d:%02d:%02d +0000" % (
        _WEEKDAYS[weekday], day, _MONTHS[month], year, hour, minute, second)


def build_message(from_addr, to_addr, subject, body):
    """ Builds a minimal RFC5322 message: headers + blank line + body.
    `body` is bytes. Sent as raw UTF-8 via 8bit/8BITMIME rather than
    quoted-printable-encoded -- that's near-universally supported on
    modern relays and avoids writing an encoder to match pop3.py's
    decoder. """
    headers = ("From: %s\r\n"
               "To: %s\r\n"
               "Subject: %s\r\n"
               "Date: %s\r\n"
               "MIME-Version: 1.0\r\n"
               "Content-Type: text/plain; charset=utf-8\r\n"
               "Content-Transfer-Encoding: 8bit\r\n"
               "\r\n" % (from_addr, to_addr, subject, _format_date()))
    return headers.encode('utf-8') + body


# ─── TUI app ──────────────────────────────────────────────────────────

def main(env, args):
    """ Creates a TUI for composing (in vi) and sending mail via SMTP. """

    host = args[0] if len(args) > 0 else None
    user = args[1] if len(args) > 1 else None
    password = args[2] if len(args) > 2 else None
    port = int(args[3]) if len(args) > 3 else 587

    if not host or not user or password is None:
        print("Usage: smtp <host> <user> <password> [port]")
        return

    tui = env.tui
    tui.enter_altscreen()
    tui.cursor_hide()

    to = ""
    subject = ""
    body = b""
    error = ""
    ui_state = "COMPOSE"

    while True:

        if ui_state == "COMPOSE":
            try:
                os.stat(_COMPOSE_PATH)  # keep an existing draft if present
            except OSError:
                try:
                    with open(_COMPOSE_PATH, "w") as f:
                        f.write("To: \nSubject: \n\n")
                except OSError as e:
                    error = "Couldn't write %s: %s" % (_COMPOSE_PATH, e)
                    ui_state = "ERROR"
                    continue

            tui.cursor_show()
            tui.clear_screen()
            env.shell.execute("vi", _COMPOSE_PATH)
            tui.cursor_hide()
            tui.enter_altscreen()

            try:
                with open(_COMPOSE_PATH, "rb") as f:
                    raw = f.read()
                headers, body = bin.pop3.parse_headers(raw)
                to = headers.get("to", "").strip()
                subject = headers.get("subject", "").strip()
            except OSError as e:
                error = "Couldn't read %s: %s" % (_COMPOSE_PATH, e)
                ui_state = "ERROR"
                continue

            if not to:
                ui_state = "INVALID"
                continue

            ui_state = "CONFIRM"

        elif ui_state == "INVALID":
            tui.clear_screen()
            bg_fill = tui.make_block("", 0, 0,
                                     width=env.cols, height=env.rows,
                                     fg=FG, bg=BG)
            bg_fill.draw()

            dlg = tui.make_dialog("Message needs a To: address.\nContinue editing?",
                                  x=env.cols // 2 - 20, y=env.rows // 2 - 3,
                                  width=40, height=6,
                                  fg=FG, bg=BG,
                                  btn1="Yes", btn2="No",
                                  sel_fg=0, sel_bg=252,
                                  title="INVALID")

            status = tui.make_label("[w/s] nav | [enter] accept",
                                    0, env.rows - 1,
                                    fg=0, bg=252,
                                    width=env.cols)

            while True:
                bg_fill.draw()
                dlg.draw()
                status.draw()
                tui.draw()

                char = sys.stdin.read(1)
                if char in ("\r", "\n"):
                    # Yes -> back to COMPOSE, draft preserved. No -> quit
                    # (draft still gets cleaned up by the QUIT state).
                    ui_state = "COMPOSE" if dlg.selected == 0 else "QUIT"
                    break
                elif char == "w":
                    dlg.left()
                elif char == "s":
                    dlg.right()

        elif ui_state == "CONFIRM":
            tui.clear_screen()
            bg_fill = tui.make_block("", 0, 0,
                                     width=env.cols, height=env.rows,
                                     fg=FG, bg=BG)
            bg_fill.draw()

            dlg = tui.make_dialog("Send to %s ?\nSubject: %s" % (to, subject or "(none)"),
                                  x=env.cols // 2 - 20, y=env.rows // 2 - 3,
                                  width=40, height=6,
                                  fg=FG, bg=BG,
                                  btn1="Yes", btn2="No",
                                  sel_fg=0, sel_bg=252,
                                  title="CONFIRM")

            status = tui.make_label("[w/s] nav | [enter] accept",
                                    0, env.rows - 1,
                                    fg=0, bg=252,
                                    width=env.cols)

            while True:
                bg_fill.draw()
                dlg.draw()
                status.draw()
                tui.draw()

                char = sys.stdin.read(1)
                if char in ("\r", "\n"):
                    ui_state = "SEND" if dlg.selected == 0 else "COMPOSE"
                    break
                elif char == "w":
                    dlg.left()
                elif char == "s":
                    dlg.right()

        elif ui_state == "SEND":
            tui.clear_screen()
            bg_fill = tui.make_block("", 0, 0,
                                     width=env.cols, height=env.rows,
                                     fg=FG, bg=BG)
            bg_fill.draw()
            tui.draw_label("Sending to %s ..." % to,
                           0, env.rows // 2,
                           fg=FG, bg=BG,
                           align="center")
            tui.draw()

            try:
                client = SMTPClient(host, port)
                client.connect(user, password)
                message = build_message(user, to, subject, body)
                client.send(user, to, message)
                client.quit()
                ui_state = "SENT"
            except Exception as e:
                # Broad on purpose: top-level connect/auth/TLS/send
                # boundary, mirrors pop3.py's LOAD state -- any failure
                # here should land on the ERROR screen, not crash out to
                # the shell's generic exception handler.
                error = str(e)
                ui_state = "ERROR"
            gc.collect()

        elif ui_state == "SENT":
            tui.clear_screen()
            bg_fill = tui.make_block("", 0, 0,
                                     width=env.cols, height=env.rows,
                                     fg=FG, bg=BG)
            bg_fill.draw()
            tui.draw_label("Message sent to %s." % to,
                           0, env.rows // 2,
                           fg=FG, bg=BG,
                           align="center")

            status = tui.make_label("[q]uit",
                                    0, env.rows - 1,
                                    fg=0, bg=252,
                                    width=env.cols)

            while True:
                status.draw()
                tui.draw()

                char = sys.stdin.read(1)
                if char == "q":
                    ui_state = "QUIT"
                    break

        elif ui_state == "ERROR":
            tui.clear_screen()
            bg_fill = tui.make_block("", 0, 0,
                                     width=env.cols, height=env.rows,
                                     fg=FG, bg=BG)
            bg_fill.draw()
            blk = tui.make_block(ERR_FG + "Error:" + CLR + "\n\n" + error,
                                 0, 1,
                                 width=env.cols, height=env.rows - 2,
                                 fg=FG, bg=BG,
                                 wrap=True)

            status = tui.make_label("[q]uit",
                                    0, env.rows - 1,
                                    fg=0, bg=252,
                                    width=env.cols)

            while True:
                blk.draw()
                status.draw()
                tui.draw()

                char = sys.stdin.read(1)
                if char == "q":
                    ui_state = "QUIT"
                    break

        elif ui_state == "QUIT":
            try:
                os.remove(_COMPOSE_PATH)  # scratch draft, don't leave it on flash
            except OSError:
                pass
            gc.collect()
            tui.exit_altscreen()
            tui.cursor_show()
            return
