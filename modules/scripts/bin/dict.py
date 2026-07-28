#
# MicroPython TUI DICT Client
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import gc
import sys
import socket

DEFAULT_HOST = "dict.org"
DEFAULT_PORT = 2628

FG = 252
BG = 18

CLR      = "\x1b[0m"
LINK_FG  = "\x1b[38;5;45m"
ERR_FG   = "\x1b[38;5;210m"
DIM      = "\x1b[38;5;244m"


class DictError(Exception):
    pass


def _tokenize(s):
    """Splits a DICT status/response line into whitespace-separated
    tokens, honoring double-quoted substrings (used for words/database
    descriptions containing spaces)."""
    tokens = []
    i = 0
    n = len(s)
    while i < n:
        while i < n and s[i] == " ":
            i += 1
        if i >= n:
            break
        if s[i] == '"':
            j = s.find('"', i + 1)
            if j < 0:
                tokens.append(s[i + 1:])
                i = n
            else:
                tokens.append(s[i + 1:j])
                i = j + 1
        else:
            j = i
            while j < n and s[j] != " ":
                j += 1
            tokens.append(s[i:j])
            i = j
    return tokens


class DictClient:
    """ Minimal RFC2229 DICT client. Talks plaintext on port 2628 -- the
    protocol has no notion of encryption. """

    def __init__(self, host, port=DEFAULT_PORT):
        self.host = host
        self.port = port
        self.sock = None

    def connect(self, timeout=15):
        ai = socket.getaddrinfo(self.host, self.port, 0, socket.SOCK_STREAM)[0]
        s = socket.socket(ai[0], socket.SOCK_STREAM, ai[2])

        if timeout is not None:
            s.settimeout(timeout)

        try:
            s.connect(ai[-1])
            self.sock = s
            code, text = self._read_status()
            if code != 220:
                raise DictError(text or "bad greeting")
        except (OSError, DictError):
            self.close()
            raise

    def _read_line(self):
        line = self.sock.readline()
        if not line:
            raise DictError("connection closed by server")
        return line.decode("utf-8", "ignore").rstrip("\r\n")

    def _read_status(self):
        """ Reads a single status line ("CODE text...") and returns
        (code, text). Raises on the 5xx/4xx-class replies that mean the
        command itself failed, so callers only need to branch on the
        success codes they actually expect. """
        line = self._read_line()
        parts = line.split(" ", 1)
        try:
            code = int(parts[0])
        except ValueError:
            raise DictError("bad response: " + line)
        text = parts[1] if len(parts) > 1 else ""
        if code >= 400:
            raise DictError(text or "server error %d" % code)
        return code, text

    def _read_multiline(self):
        """ Reads a dot-terminated multiline block, undoing byte-stuffing
        (a leading '..' on a line means a literal '.', not the
        end-of-block marker) -- same convention as POP3/SMTP. """
        lines = []
        while True:
            line = self.sock.readline()
            if not line:
                raise DictError("connection closed mid-response")
            if line == b".\r\n" or line == b".\n":
                break
            if line.startswith(b".."):
                line = line[1:]
            lines.append(line)
        return b"".join(lines)

    def define(self, word, database="*"):
        """ Returns a list of (db, dbdesc, text) definitions. Empty list
        means the word wasn't found in any database (status 552), which
        is a normal outcome, not an error. """
        self.sock.write('DEFINE %s "%s"\r\n' % (database, word))
        code, text = self._read_status()
        if code == 552:
            return []
        if code != 150:
            raise DictError(text or "unexpected response %d" % code)

        defs = []
        while True:
            code, text = self._read_status()
            if code != 151:
                break
            tokens = _tokenize(text)
            db = tokens[1] if len(tokens) > 1 else ""
            dbdesc = tokens[2] if len(tokens) > 2 else db
            body = self._read_multiline()
            defs.append((db, dbdesc, str(body, "utf-8", "ignore")))

        if code != 250:
            raise DictError(text or "unexpected response %d" % code)
        return defs

    def match(self, word, database="*", strategy="."):
        """ Returns a list of (db, word) suggestions using the given
        match strategy ("." = server default, usually exact/prefix).
        Empty list means no matches (status 552). """
        self.sock.write('MATCH %s %s "%s"\r\n' % (database, strategy, word))
        code, text = self._read_status()
        if code == 552:
            return []
        if code != 152:
            raise DictError(text or "unexpected response %d" % code)

        body = self._read_multiline()
        code, text = self._read_status()
        if code != 250:
            raise DictError(text or "unexpected response %d" % code)

        matches = []
        for line in str(body, "utf-8", "ignore").split("\r\n"):
            line = line.strip()
            if not line:
                continue
            tokens = _tokenize(line)
            if len(tokens) >= 2:
                matches.append((tokens[0].strip(), tokens[1].strip()))
        return matches

    def quit(self):
        try:
            self.sock.write("QUIT\r\n")
            self._read_status()
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


def _render_definitions(defs):
    """ Flattens [(db, dbdesc, text), ...] into display lines, with a
    highlighted header ahead of each database's entry. Server text
    commonly comes padded with leading/trailing whitespace per line
    (and blank lines around the body), which reads as odd, ragged
    indentation in a fixed-width TUI -- strip it all before display. """
    lines = []
    for db, dbdesc, text in defs:
        if lines:
            lines.append("")
        lines.append(LINK_FG + "-- " + (dbdesc or db).strip() + " --" + CLR)
        body = text.split("\r\n") if "\r\n" in text else text.split("\n")
        body = [line.strip() for line in body]
        while body and not body[0]:
            body.pop(0)
        while body and not body[-1]:
            body.pop()
        lines.extend(body)
    return lines


def main(env, args):
    """ Creates a TUI for looking up word definitions via DICT (RFC2229). """

    host = args[0] if len(args) > 0 else DEFAULT_HOST
    port = int(args[1]) if len(args) > 1 else DEFAULT_PORT
    database = args[2] if len(args) > 2 else "*"

    tui = env.tui
    tui.enter_altscreen()
    tui.cursor_hide()

    client = DictClient(host, port)
    connected = False
    word = ""
    display = []
    suggestions = []
    error = ""
    ui_state = "INPUT"

    while True:

        if ui_state == "INPUT":
            tui.clear_screen()
            bg_fill = tui.make_block("", 0, 0,
                                     width=env.cols, height=env.rows,
                                     fg=FG, bg=BG)
            bg_fill.draw()
            label = tui.make_label("Define word on %s:" % host,
                                   0, 1,
                                   fg=FG, bg=BG,
                                   align="center")

            word_input = tui.make_input("> ",
                                 0, 3,
                                 width=env.cols-2,
                                 fg=FG, bg=BG, input_bg=0,
                                 decorations=False,
                                 align="center")

            status = tui.make_label("[esc] %s | [enter] look up" %
                                    ("quit" if not display and not suggestions else "cancel"),
                                    0, env.rows-1,
                                    fg=0, bg=252,
                                    width=env.cols)

            tui.cursor_show()
            label.draw()
            status.draw()
            word_input.draw()

            while ui_state == "INPUT":
                char = sys.stdin.read(1)
                if char == "\n" or char == "\r":
                    if word_input.value:
                        tui.cursor_hide()
                        word = word_input.value
                        ui_state = "LOAD"
                        break
                elif char in ("\x08", "\x7f"):
                    word_input.backspace()
                elif char == "\x1b":
                    tui.cursor_hide()
                    ui_state = "PAGE" if display else "SUGGEST" if suggestions else "QUIT"
                    break
                else:
                    word_input.push(char)
                word_input.draw()
                tui.draw()

        elif ui_state == "LOAD":
            tui.clear_screen()
            bg_fill = tui.make_block("", 0, 0,
                                     width=env.cols, height=env.rows,
                                     fg=FG, bg=BG)
            bg_fill.draw()
            tui.draw_label("Looking up '%s' on %s ..." % (word, host),
                           0, env.rows // 2,
                           fg=FG, bg=BG,
                           align="center")
            tui.draw()

            try:
                if not connected:
                    client.connect()
                    connected = True
                defs = client.define(word, database)
                if defs:
                    display = _render_definitions(defs)
                    suggestions = []
                    ui_state = "PAGE"
                else:
                    matches = client.match(word, database)
                    display = []
                    if matches:
                        suggestions = matches
                        ui_state = "SUGGEST"
                    else:
                        error = "No definition found for '%s'." % word
                        ui_state = "ERROR"
            except Exception as e:
                error = str(e)
                connected = False
                ui_state = "ERROR"
            gc.collect()

        elif ui_state == "PAGE":
            tui.clear_screen()
            status = tui.make_label("[w/s] nav | [n]ew word | [q]uit",
                                    0, env.rows-1,
                                    fg=0, bg=252,
                                    width=env.cols)

            lst = tui.make_list(display if display else ["(no definition)"],
                                x=0, y=0,
                                width=env.cols, height=env.rows-1,
                                fg=FG, bg=BG,
                                arrow=">", left_pad=1,
                                multiline=True, wrap=True)

            while True:
                lst.draw()
                status.draw()
                tui.draw()

                char = sys.stdin.read(1)
                if char == "w":
                    lst.up()
                elif char == "s":
                    lst.down()
                elif char == "n":
                    ui_state = "INPUT"
                    break
                elif char == "q":
                    ui_state = "QUIT"
                    break

        elif ui_state == "SUGGEST":
            tui.clear_screen()
            status = tui.make_label("[w/s] nav | [enter] define | [n]ew word | [q]uit",
                                    0, env.rows-1,
                                    fg=0, bg=252,
                                    width=env.cols)

            lines = [DIM + db + ": " + CLR + w for db, w in suggestions]

            lst = tui.make_list(lines,
                                x=0, y=0,
                                width=env.cols, height=env.rows-1,
                                fg=FG, bg=BG,
                                arrow=">", left_pad=1,
                                multiline=True, wrap=True)

            while True:
                lst.draw()
                status.draw()
                tui.draw()

                char = sys.stdin.read(1)
                if char == "\n" or char == "\r":
                    word = suggestions[lst.index][1]
                    ui_state = "LOAD"
                    break
                elif char == "w":
                    lst.up()
                elif char == "s":
                    lst.down()
                elif char == "n":
                    ui_state = "INPUT"
                    break
                elif char == "q":
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
                                 width=env.cols, height=env.rows-2,
                                 fg=FG, bg=BG,
                                 wrap=True)

            status = tui.make_label("[n]ew word | [q]uit",
                                    0, env.rows-1,
                                    fg=0, bg=252,
                                    width=env.cols)

            while True:
                blk.draw()
                status.draw()
                tui.draw()

                char = sys.stdin.read(1)
                if char == "n":
                    ui_state = "INPUT"
                    break
                elif char == "q":
                    ui_state = "QUIT"
                    break

        elif ui_state == "QUIT":
            try:
                if connected:
                    client.quit()
            except (OSError, DictError):
                client.close()
            gc.collect()
            tui.exit_altscreen()
            tui.cursor_show()
            return
