#
# MicroPython TUI Gopher Browser
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import gc
import sys
import socket

DEFAULT_PORT = 70

FG = 252
BG = 18

# A plain color index, not an escape string -- make_pager() owns link
# coloring itself now rather than taking pre-colored text.
LINK_COLOR = 45

CLR      = "\x1b[0m"
ERR_FG   = "\x1b[38;5;210m"
QUOTE_FG = "\x1b[38;5;244m"

# Gopher item-type character -> our internal "kind". Anything not listed
# here (images, binaries, telnet sessions, HTML gateways, ...) falls back
# to "other" -- this is a text client with no way to usefully display them.
_KIND_MAP = {
    "0": "text",   # plain text file
    "1": "menu",   # directory listing (submenu)
    "7": "search", # index/search server -- needs a query before fetching
    "i": "info",   # informational line, not a link
    "3": "error",  # server-signaled error line
}


class GopherError(Exception):
    pass


class Response:
    def __init__(self, sock, itemtype):
        self._sock = sock
        self.itemtype = itemtype
        self._cached = None

    def close(self):
        if self._sock:
            self._sock.close()
            self._sock = None

    @property
    def content(self):
        # Gopher responses have no length header -- they just run until
        # the server closes the connection, so read until EOF.
        if self._cached is None:
            try:
                chunks = []
                while True:
                    b = self._sock.read(512)
                    if not b:
                        break
                    chunks.append(b)
                self._cached = b"".join(chunks)
            finally:
                self.close()
        return self._cached

    @property
    def text(self):
        return str(self.content, "utf-8")

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()


def _parse_url(url):
    """Parse gopher://host[:port]/[TYPE]selector into (host, port, itemtype, selector).

    With no path, defaults to itemtype "1" (directory) and an empty
    selector -- the conventional way to address a server's root menu.
    """
    if url.startswith("gopher://"):
        url = url[len("gopher://") :]
    elif "://" in url:
        raise GopherError("Unsupported protocol: " + url.split("://", 1)[0])

    if "/" in url:
        hostport, rest = url.split("/", 1)
    else:
        hostport, rest = url, ""

    if ":" in hostport:
        host, port = hostport.split(":", 1)
        port = int(port)
    else:
        host, port = hostport, DEFAULT_PORT

    if rest:
        itemtype = rest[0]
        selector = rest[1:]
    else:
        itemtype = "1"
        selector = ""

    return host, port, itemtype, selector


def _build_url(host, port, itemtype, selector):
    hostport = host if port == DEFAULT_PORT else "%s:%d" % (host, port)
    return "gopher://%s/%s%s" % (hostport, itemtype, selector)


def request(url, query=None, timeout=15):
    """Issue a single Gopher request and return a Response.

    Caller must close() the response (or use it as a context manager)
    once done with .content/.text -- the socket is kept open until then
    since Gopher bodies are EOF-terminated rather than length-prefixed.
    """
    host, port, itemtype, selector = _parse_url(url)

    line = selector if query is None else selector + "\t" + query

    ai = socket.getaddrinfo(host, port, 0, socket.SOCK_STREAM)[0]
    s = socket.socket(ai[0], socket.SOCK_STREAM, ai[2])

    if timeout is not None:
        s.settimeout(timeout)

    try:
        s.connect(ai[-1])
        s.write(line)
        s.write("\r\n")
        return Response(s, itemtype)
    except OSError:
        s.close()
        raise


def get(url, query=None, timeout=15):
    """Fetch url. query, if given, is tab-appended to the selector (the
    Gopher convention for submitting to a type-7 search server)."""
    return request(url, query=query, timeout=timeout)


def parse_gophermenu(text):
    """Parse a Gopher directory listing into a list of
    (kind, text, host, port, selector, itemtype) lines.

    kind is one of: "text", "menu", "search", "info", "error", "other".
    A line consisting of a lone "." ends the listing (per RFC 1436);
    blank lines are kept as empty "info" lines to preserve spacing.
    """
    lines = []

    for raw in text.split("\n"):
        line = raw[:-1] if raw.endswith("\r") else raw

        if line == ".":
            break
        if not line:
            lines.append(("info", "", "", 0, "", "i"))
            continue

        itemtype = line[0]
        parts = line[1:].split("\t")
        display = parts[0] if len(parts) > 0 else ""
        selector = parts[1] if len(parts) > 1 else ""
        host = parts[2] if len(parts) > 2 else ""
        try:
            port = int(parts[3]) if len(parts) > 3 else 0
        except ValueError:
            port = 0

        kind = _KIND_MAP.get(itemtype, "other")
        lines.append((kind, display, host, port, selector, itemtype))

    return lines


def _render_lines(parsed):
    """Turn parse_gophermenu() output into (text, target) segments for
    tui.make_pager(). target is a (host, port, itemtype, selector) tuple
    for navigable lines, or None for "info"/"error"/"other" lines.

    Each gophermap line is still exactly one row -- a trailing '\\n' on
    every entry forces the pager to break there instead of reflowing
    consecutive short lines onto the same row. Link coloring is the
    pager's job now (link_fg/cur_fg), but "error"/"other" aren't
    navigable so their color is still embedded directly, same as before.
    """
    out = []
    for kind, text, host, port, selector, itemtype in parsed:
        target = (host, port, itemtype, selector)
        if kind == "menu":
            out.append(("-> " + text + "\n", target))
        elif kind == "text":
            out.append(("-- " + text + "\n", target))
        elif kind == "search":
            out.append(("? " + text + "\n", target))
        elif kind == "error":
            out.append((ERR_FG + "! " + text + CLR + "\n", None))
        elif kind == "other":
            out.append((QUOTE_FG + "[ ] " + text + CLR + "\n", None))
        else:  # info
            out.append((text + "\n", None))
    return out


def _load(url, query=None):
    """Fetch url and return (status_kind, payload).

    status_kind is one of "page", "error". payload is a list of
    (text, target) segments (see _render_lines) for "page", or an
    error message for "error".
    """
    try:
        resp = get(url, query=query)
    except Exception as e:
        return "error", str(e)

    try:
        itemtype = resp.itemtype
        if itemtype in ("1", "7"):
            # A search server's results come back directory-formatted,
            # same as a plain type-1 menu.
            parsed = parse_gophermenu(resp.text)
            return "page", _render_lines(parsed)
        if itemtype == "0":
            lines = resp.text.split("\n")
            if lines and lines[-1] == ".":
                lines = lines[:-1]
            return "page", [(line + "\n", None) for line in lines]
        return "error", "Unsupported item type: " + itemtype
    finally:
        resp.close()


def main(env, args):
    """ Creates a TUI for browsing gopher:// pages """

    url = args[0] if len(args) > 0 else None
    if not url:
        print("Usage: gopher <url>")
        return
    if "://" not in url:
        url = "gopher://" + url

    tui = env.tui
    tui.enter_altscreen()
    tui.cursor_hide()

    history = []
    segments = []
    prompt = ""
    error = ""
    pending_query = None

    # A type-7 (search) target needs a query collected *before* the first
    # request is ever made -- unlike Gemini, there's no server round trip
    # to tell us input is wanted.
    _, _, start_itemtype, _ = _parse_url(url)
    if start_itemtype == "7":
        prompt = "Search query:"
        ui_state = "INPUT"
    else:
        ui_state = "LOAD"

    while True:

        if ui_state == "LOAD":
            tui.clear_screen()
            bg_fill = tui.make_block("", 0, 0,
                                     width=env.cols, height=env.rows,
                                     fg=FG, bg=BG)
            bg_fill.draw()
            tui.draw_label("Loading %s ..." % url,
                           0, env.rows // 2,
                           fg=FG, bg=BG,
                           align="center")
            tui.draw()

            kind, payload = _load(url, query=pending_query)
            pending_query = None
            gc.collect()

            if kind == "page":
                segments = payload
                ui_state = "PAGE"
            else:
                error = payload
                ui_state = "ERROR"

        elif ui_state == "PAGE":
            tui.clear_screen()
            status = tui.make_label("[w/s] nav | [n/N] links | [enter] open | [b]ack | [q]uit",
                                    0, env.rows-1,
                                    fg=0, bg=252,
                                    width=env.cols)

            pager = tui.make_pager(segments if segments else [("(empty page)", None)],
                                   0, 0,
                                   width=env.cols, height=env.rows-1,
                                   fg=FG, bg=BG,
                                   link_fg=LINK_COLOR, link_bg=BG,
                                   cur_fg=BG, cur_bg=LINK_COLOR)

            while True:
                pager.draw()
                status.draw()
                tui.draw()

                char = sys.stdin.read(1)
                if char == "\n" or char == "\r":
                    target = pager.current_link
                    if target is not None:
                        host, port, itemtype, selector = target
                        history.append(url)
                        url = _build_url(host, port, itemtype, selector)
                        if itemtype == "7":
                            prompt = "Search query:"
                            ui_state = "INPUT"
                        else:
                            ui_state = "LOAD"
                        break
                elif char == "w":
                    pager.up()
                elif char == "s":
                    pager.down()
                elif char == "n":
                    pager.next_link()
                elif char == "N":
                    pager.prev_link()
                elif char == "b":
                    if history:
                        url = history.pop()
                        ui_state = "LOAD"
                        break
                elif char == "q":
                    ui_state = "QUIT"
                    break

        elif ui_state == "INPUT":
            tui.clear_screen()
            bg_fill = tui.make_block("", 0, 0,
                                     width=env.cols, height=env.rows,
                                     fg=FG, bg=BG)
            bg_fill.draw()
            label = tui.make_label(prompt,
                                   0, 1,
                                   fg=FG, bg=BG,
                                   align="center")

            ans_input = tui.make_input("> ",
                                 0, 3,
                                 width=env.cols-2,
                                 fg=FG, bg=BG, input_bg=0,
                                 decorations=False,
                                 align="center")

            status = tui.make_label("[esc] cancel | [enter] submit",
                                    0, env.rows-1,
                                    fg=0, bg=252,
                                    width=env.cols)

            tui.cursor_show()
            label.draw()
            status.draw()
            ans_input.draw()

            while ui_state == "INPUT":
                char = sys.stdin.read(1)
                if char == "\n" or char == "\r":
                    tui.cursor_hide()
                    pending_query = ans_input.value
                    ui_state = "LOAD"
                    break
                elif char in ("\x08", "\x7f"):
                    ans_input.backspace()
                elif char == "\x1b":
                    tui.cursor_hide()
                    ui_state = "PAGE"
                    break
                else:
                    ans_input.push(char)
                ans_input.draw()
                tui.draw()

        elif ui_state == "ERROR":
            tui.clear_screen()
            bg_fill = tui.make_block("", 0, 0,
                                     width=env.cols, height=env.rows,
                                     fg=FG, bg=BG)
            bg_fill.draw()
            blk = tui.make_block(ERR_FG + "Error loading page:" + CLR + "\n\n" + error,
                                 0, 1,
                                 width=env.cols, height=env.rows-2,
                                 fg=FG, bg=BG,
                                 wrap=True)

            status = tui.make_label("[b]ack | [q]uit",
                                    0, env.rows-1,
                                    fg=0, bg=252,
                                    width=env.cols)

            while True:
                blk.draw()
                status.draw()
                tui.draw()

                char = sys.stdin.read(1)
                if char == "b":
                    if history:
                        url = history.pop()
                        ui_state = "LOAD"
                        break
                elif char == "q":
                    ui_state = "QUIT"
                    break

        elif ui_state == "QUIT":
            gc.collect()
            tui.exit_altscreen()
            tui.cursor_show()
            return
