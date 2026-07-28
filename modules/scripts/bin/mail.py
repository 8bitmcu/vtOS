#
# MicroPython TUI POP3 Email Client
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import gc
import os
import sys
import socket
import binascii

FG = 252
BG = 18

CLR = "\x1b[0m"
DIM = "\x1b[38;5;244m"
ERR_FG = "\x1b[38;5;210m"

_VIEW_PATH = "/flash/.view.mail"


class POP3Error(Exception):
    pass


class POP3Client:
    """ Minimal RFC1939 POP3 client. Talks plaintext on port 110 or
    implicit TLS (the de-facto standard for real providers like Gmail/
    Outlook -- there's no STLS/STARTTLS support here) on any other port,
    port 995 by default. """

    def __init__(self, host, port=995, use_tls=None):
        self.host = host
        self.port = port
        self.use_tls = (port != 110) if use_tls is None else use_tls
        self.sock = None

    def connect(self, user, password, timeout=15):
        ai = socket.getaddrinfo(self.host, self.port, 0, socket.SOCK_STREAM)[0]
        s = socket.socket(ai[0], socket.SOCK_STREAM, ai[2])

        if timeout is not None:
            # Must be set before wrap_socket() -- the returned SSLSocket
            # doesn't expose settimeout() of its own on this port.
            s.settimeout(timeout)

        try:
            s.connect(ai[-1])

            if self.use_tls:
                import tls

                # No cert store to validate against on this device, same
                # tradeoff gemini.py/requests.py already make for TLS.
                context = tls.SSLContext(tls.PROTOCOL_TLS_CLIENT)
                context.verify_mode = tls.CERT_NONE
                s = context.wrap_socket(s, server_hostname=self.host)

            self.sock = s

            greeting = self.sock.readline()
            if not greeting or not greeting.startswith(b'+OK'):
                raise POP3Error("Bad greeting: %r" % greeting)

            self._send_cmd("USER " + user)
            self._send_cmd("PASS " + password)
        except (OSError, POP3Error):
            self.close()
            raise

    def _send_cmd(self, cmd):
        """ Sends a command and returns the status line (without the
        leading '+OK '/'-ERR '). Raises POP3Error on a '-ERR' reply. """
        self.sock.write(cmd + "\r\n")
        resp = self.sock.readline()
        if not resp:
            raise POP3Error("connection closed by server")
        resp = resp.decode('utf-8', 'ignore').rstrip("\r\n")
        if resp.startswith("-ERR"):
            raise POP3Error(resp[5:] if len(resp) > 5 else "error")
        if not resp.startswith("+OK"):
            raise POP3Error("Unexpected response: " + resp)
        return resp[4:] if len(resp) > 4 else ""

    def _read_multiline(self):
        """ Reads a dot-terminated multiline block (LIST/TOP/RETR bodies),
        undoing RFC1939 byte-stuffing (a leading '..' on a line means a
        literal '.', not the end-of-block marker). """
        lines = []
        while True:
            line = self.sock.readline()
            if not line:
                raise POP3Error("connection closed mid-response")
            if line == b".\r\n" or line == b".\n":
                break
            if line.startswith(b".."):
                line = line[1:]
            lines.append(line)
        return b"".join(lines)

    def stat(self):
        """ Returns (message_count, mailbox_size_bytes). """
        resp = self._send_cmd("STAT")
        count, size = resp.strip().split()
        return int(count), int(size)

    def list(self):
        """ Returns [(msgnum, size), ...] for every non-deleted message. """
        self._send_cmd("LIST")
        raw = self._read_multiline()
        out = []
        for line in raw.decode('utf-8', 'ignore').split("\r\n"):
            line = line.strip()
            if not line:
                continue
            num, size = line.split()
            out.append((int(num), int(size)))
        return out

    def top(self, msgnum, lines=0):
        """ Returns the headers (and up to `lines` body lines) of a
        message as raw bytes, without marking it as retrieved. """
        self._send_cmd("TOP %d %d" % (msgnum, lines))
        return self._read_multiline()

    def retr(self, msgnum):
        """ Returns a full message as raw bytes. """
        self._send_cmd("RETR %d" % msgnum)
        return self._read_multiline()

    def dele(self, msgnum):
        """ Marks a message for deletion -- only actually removed once
        quit() commits it. """
        self._send_cmd("DELE %d" % msgnum)

    def quit(self):
        """ Commits any pending DELEs and closes the connection. An
        unclean disconnect (socket error, close() without quit())
        implicitly RSETs on the server per RFC1939 -- so this must be
        called for deletes to stick. """
        try:
            self._send_cmd("QUIT")
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


# ─── MIME / header parsing (pure functions, no I/O) ─────────────────────

def parse_headers(raw):
    """ Splits a raw RFC822 message into (headers_dict, body_bytes).

    Header field names are lowercased. Folded continuation lines (ones
    starting with whitespace) are joined onto the previous field, per
    RFC822 unfolding rules. """
    if b"\r\n\r\n" in raw:
        head, body = raw.split(b"\r\n\r\n", 1)
    elif b"\n\n" in raw:
        head, body = raw.split(b"\n\n", 1)
    else:
        head, body = raw, b""

    headers = {}
    last_key = None
    for raw_line in head.replace(b"\r\n", b"\n").split(b"\n"):
        if not raw_line:
            continue
        line = raw_line.decode('utf-8', 'ignore')
        if line[0] in (" ", "\t") and last_key:
            headers[last_key] += " " + line.strip()
            continue
        if ":" not in line:
            continue
        key, val = line.split(":", 1)
        last_key = key.strip().lower()
        headers[last_key] = val.strip()

    return headers, body


def _qp_decode(data):
    """ Decodes quoted-printable bytes. MicroPython's binascii has no
    a2b_qp, so this is a small hand-rolled decoder: '=XX' is a literal
    byte, a trailing '=' at end of line is a soft line break (join with
    the next line), anything else passes through unchanged. """
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        b = data[i]
        if b == 0x3D:  # '='
            if data[i+1:i+3] == b"\r\n":
                i += 3  # soft line break
                continue
            if data[i+1:i+2] == b"\n":
                i += 2  # bare-LF soft line break
                continue
            hexpair = data[i+1:i+3]
            if len(hexpair) == 2:
                try:
                    out.append(int(hexpair, 16))
                    i += 3
                    continue
                except ValueError:
                    pass
            out.append(b)
            i += 1
        else:
            out.append(b)
            i += 1
    return bytes(out)


def decode_rfc2047(s):
    """ Decodes RFC2047 encoded-words ("=?charset?B?...?=" / "=?charset?Q?
    ...?=") that show up in Subject/From on real-world mail. Unknown or
    malformed encoded-words are left as-is rather than raising -- a
    garbled subject is better than a crashed app. """
    out = []
    i = 0
    n = len(s)
    while i < n:
        start = s.find("=?", i)
        if start < 0:
            out.append(s[i:])
            break
        out.append(s[i:start])

        end = s.find("?=", start + 2)
        if end < 0:
            out.append(s[start:])
            break

        word = s[start + 2:end]
        parts = word.split("?", 2)
        if len(parts) != 3:
            out.append(s[start:end + 2])
            i = end + 2
            continue

        charset, enc, text = parts
        try:
            if enc.upper() == "B":
                decoded = binascii.a2b_base64(text)
            elif enc.upper() == "Q":
                decoded = _qp_decode(text.replace("_", " ").encode('latin-1'))
            else:
                raise ValueError("unknown encoding")
            out.append(str(decoded, charset, 'ignore') if charset.lower() != "utf-8"
                       else str(decoded, 'utf-8', 'ignore'))
        except Exception:
            out.append(s[start:end + 2])

        i = end + 2

    return "".join(out)


def decode_body(data, transfer_encoding):
    """ Decodes a MIME part's body per its Content-Transfer-Encoding. """
    enc = (transfer_encoding or "").strip().lower()
    if enc == "base64":
        try:
            # Servers/clients often wrap base64 at 76 cols; a2b_base64
            # wants it unwrapped.
            return binascii.a2b_base64(data.replace(b"\r\n", b"").replace(b"\n", b""))
        except Exception:
            return data
    if enc == "quoted-printable":
        return _qp_decode(data)
    return data


def _parse_content_type(value):
    """ Splits a Content-Type (or Content-Disposition) header value into
    (main_value_lowercased, {param: value}). """
    if not value:
        return "", {}
    parts = value.split(";")
    main = parts[0].strip().lower()
    params = {}
    for p in parts[1:]:
        if "=" not in p:
            continue
        k, v = p.split("=", 1)
        params[k.strip().lower()] = v.strip().strip('"')
    return main, params


def _strip_html(text):
    """ Crude tag stripper for text/html fallback -- no real rendering,
    just enough to make an HTML-only email readable in a text UI. """
    out = []
    in_tag = False
    for ch in text:
        if ch == "<":
            in_tag = True
        elif ch == ">":
            in_tag = False
        elif not in_tag:
            out.append(ch)
    result = "".join(out)
    for src, dst in (("&nbsp;", " "), ("&amp;", "&"), ("&lt;", "<"),
                      ("&gt;", ">"), ("&quot;", '"'), ("&#39;", "'")):
        result = result.replace(src, dst)
    return result


def find_best_text_part(headers, body):
    """ Walks a (possibly multipart) message body and returns
    (text, attachments) -- text is the best renderable text found
    (text/plain preferred, text/html stripped-down as fallback),
    attachments is a list of filenames seen along the way (for display
    only, nothing is extracted). """
    ctype, params = _parse_content_type(headers.get("content-type", ""))

    if not ctype.startswith("multipart/"):
        cte = headers.get("content-transfer-encoding", "")
        decoded = decode_body(body, cte)
        charset = params.get("charset", "utf-8")
        try:
            text = str(decoded, charset, 'ignore')
        except Exception:
            text = str(decoded, 'utf-8', 'ignore')
        if ctype == "text/html":
            text = _strip_html(text)
        return text, []

    boundary = params.get("boundary")
    if not boundary:
        return "(multipart message with no boundary)", []

    delim = ("--" + boundary).encode('utf-8')
    raw_parts = body.split(delim)

    best_plain = None
    best_html = None
    attachments = []

    for raw_part in raw_parts[1:]:
        if raw_part.startswith(b"--"):
            continue  # final delimiter
        raw_part = raw_part.lstrip(b"\r\n").lstrip(b"\n")
        part_headers, part_body = parse_headers(raw_part)

        part_ctype, part_params = _parse_content_type(part_headers.get("content-type", ""))
        disp, disp_params = _parse_content_type(part_headers.get("content-disposition", ""))
        filename = disp_params.get("filename") or part_params.get("name")
        if disp == "attachment" and filename:
            attachments.append(filename)
            continue

        if part_ctype.startswith("multipart/"):
            text, nested_attach = find_best_text_part(part_headers, part_body)
            attachments.extend(nested_attach)
            if best_plain is None:
                best_plain = text
            continue

        cte = part_headers.get("content-transfer-encoding", "")
        decoded = decode_body(part_body, cte)
        charset = part_params.get("charset", "utf-8")
        try:
            text = str(decoded, charset, 'ignore')
        except Exception:
            text = str(decoded, 'utf-8', 'ignore')

        if part_ctype == "text/plain" and best_plain is None:
            best_plain = text
        elif part_ctype == "text/html" and best_html is None:
            best_html = _strip_html(text)

    if best_plain is not None:
        return best_plain, attachments
    if best_html is not None:
        return best_html, attachments
    return "(no readable text part found)", attachments


def _format_msg_line(headers):
    frm = decode_rfc2047(headers.get("from", "(unknown sender)"))
    subj = decode_rfc2047(headers.get("subject", "(no subject)"))
    return frm + "  " + DIM + subj + CLR


def main(env, args):
    """ Creates a TUI for reading (and deleting) POP3 mail. """

    host = args[0] if len(args) > 0 else None
    user = args[1] if len(args) > 1 else None
    password = args[2] if len(args) > 2 else None
    port = int(args[3]) if len(args) > 3 else 995

    if not host or not user or password is None:
        print("Usage: mail <host> <user> <password> [port]")
        return

    tui = env.tui
    tui.enter_altscreen()
    tui.cursor_hide()

    client = POP3Client(host, port)
    messages = []   # [(msgnum, headers_dict)]
    body_cache = {}  # msgnum -> (text, attachments)
    error = ""
    selected = 0
    ui_state = "LOAD"

    while True:

        if ui_state == "LOAD":
            tui.clear_screen()
            bg_fill = tui.make_block("", 0, 0,
                                     width=env.cols, height=env.rows,
                                     fg=FG, bg=BG)
            bg_fill.draw()
            tui.draw_label("Connecting to %s ..." % host,
                           0, env.rows // 2,
                           fg=FG, bg=BG,
                           align="center")
            tui.draw()

            try:
                client.connect(user, password)
                count, total_size = client.stat()
                listing = client.list()
                messages = []
                for i, (num, size) in enumerate(listing):
                    tui.draw_label("Fetching message %d/%d (%d msgs, %d KB total) ..." %
                                   (i + 1, len(listing), count, total_size // 1024),
                                   0, env.rows // 2 + 1,
                                   fg=FG, bg=BG,
                                   align="center")
                    tui.draw()
                    raw = client.top(num, 0)
                    headers, _ = parse_headers(raw)
                    messages.append((num, headers))
                ui_state = "INBOX"
            except Exception as e:
                error = str(e)
                ui_state = "ERROR"
            gc.collect()

        elif ui_state == "INBOX":
            tui.clear_screen()
            status = tui.make_label("[w/s] nav | [d]elete | [q]uit",
                                    0, env.rows - 1,
                                    fg=0, bg=252,
                                    width=env.cols)

            lines = [_format_msg_line(h) for _, h in messages] if messages else ["(no messages)"]
            lst = tui.make_list(lines,
                                x=0, y=0,
                                width=env.cols, height=env.rows - 1,
                                fg=FG, bg=BG,
                                arrow=">", left_pad=1,
                                multiline=True, wrap=True)
            lst.index = min(selected, max(len(lines) - 1, 0))

            while True:
                lst.draw()
                status.draw()
                tui.draw()

                char = sys.stdin.read(1)
                if (char == "\n" or char == "\r") and messages:
                    selected = lst.index
                    ui_state = "READ"
                    break
                elif char == "w":
                    lst.up()
                elif char == "s":
                    lst.down()
                elif char == "d" and messages:
                    selected = lst.index
                    ui_state = "DELETE_CONFIRM"
                    break
                elif char == "q":
                    ui_state = "QUIT"
                    break

        elif ui_state == "READ":
            num, headers = messages[selected]

            if num in body_cache:
                text, attachments = body_cache[num]
            else:
                try:
                    raw = client.retr(num)
                    body_headers, body = parse_headers(raw)
                    text, attachments = find_best_text_part(body_headers, body)
                except Exception as e:
                    text, attachments = "(failed to fetch message: %s)" % e, []
                body_cache[num] = (text, attachments)
                gc.collect()

            header_lines = []
            # str.title() isn't implemented in MicroPython, hence the
            # explicit labels rather than field.title().
            for field, label in (("from", "From"), ("subject", "Subject"),
                                 ("to", "To"), ("cc", "Cc")):
                value = headers.get(field, "")
                if value:
                    header_lines.append(label + ": " + decode_rfc2047(value))
            date = headers.get("date", "")
            if date:
                header_lines.append("Date: " + date)  # not RFC2047-encoded
            if attachments:
                header_lines.append("Attachments: " + ", ".join(attachments))

            content = "\n".join(header_lines) + "\n\n" + text

            try:
                with open(_VIEW_PATH, "w") as f:
                    f.write(content)
            except OSError as e:
                error = "Couldn't write %s: %s" % (_VIEW_PATH, e)
                ui_state = "ERROR"
                continue

            tui.cursor_show()
            tui.clear_screen()
            env.shell.execute("vi", _VIEW_PATH)
            tui.cursor_hide()
            tui.enter_altscreen()
            ui_state = "INBOX"

        elif ui_state == "DELETE_CONFIRM":
            tui.clear_screen()
            bg_fill = tui.make_block("", 0, 0,
                                     width=env.cols, height=env.rows,
                                     fg=FG, bg=BG)
            bg_fill.draw()

            dlg = tui.make_dialog("Delete this message?",
                                  x=env.cols // 2 - 20, y=env.rows // 2 - 2,
                                  width=40, height=5,
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
                    if dlg.selected == 0:  # "Yes"
                        num, _ = messages[selected]
                        try:
                            client.dele(num)
                            del messages[selected]
                            body_cache.pop(num, None)
                            selected = min(selected, max(len(messages) - 1, 0))
                        except (OSError, POP3Error) as e:
                            error = str(e)
                            ui_state = "ERROR"
                            break
                    ui_state = "INBOX"
                    break
                elif char == "w":
                    dlg.left()
                elif char == "s":
                    dlg.right()

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
                client.quit()
            except (OSError, POP3Error):
                client.close()
            try:
                os.remove(_VIEW_PATH)  # scratch decode, don't leave it on flash
            except OSError:
                pass
            gc.collect()
            tui.exit_altscreen()
            tui.cursor_show()
            return
