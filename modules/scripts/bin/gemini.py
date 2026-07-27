#
# MicroPython TUI Gemini Browser
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import gc
import sys
import socket
import tls

DEFAULT_PORT = 1965

# Full response header line ("<status><space><meta>\r\n") is capped at
# 1024 bytes by the spec; keep the request URL under that too.
MAX_HEADER_LEN = 1024

FG = 252
BG = 18

CLR      = "\x1b[0m"
H1_FG    = "\x1b[1;38;5;255m"
H2_FG    = "\x1b[1;38;5;250m"
H3_FG    = "\x1b[38;5;250m"
LINK_FG  = "\x1b[38;5;45m"
QUOTE_FG = "\x1b[38;5;244m"
PRE_FG   = "\x1b[38;5;244m"
ERR_FG   = "\x1b[38;5;210m"

_QUOTE_SAFE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~"


class GeminiError(Exception):
    pass


class Response:
    def __init__(self, sock, status, meta):
        self._sock = sock
        self.status = status  # int, e.g. 20, 31, 51
        self.meta = meta  # str: mimetype (2x), prompt/url/errmsg otherwise
        self._cached = None

    @property
    def category(self):
        # 1 input, 2 success, 3 redirect, 4 temp fail, 5 perm fail, 6 cert
        return self.status // 10

    def close(self):
        if self._sock:
            self._sock.close()
            self._sock = None

    @property
    def content(self):
        # Gemini bodies have no Content-Length -- they just run until the
        # server closes the connection, so read until EOF.
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
    if url.startswith("gemini://"):
        url = url[len("gemini://") :]
    elif "://" in url:
        raise GeminiError("Unsupported protocol: " + url.split("://", 1)[0])

    if "/" in url:
        hostport, path = url.split("/", 1)
        path = "/" + path
    else:
        hostport, path = url, "/"

    if ":" in hostport:
        host, port = hostport.split(":", 1)
        port = int(port)
    else:
        host, port = hostport, DEFAULT_PORT

    return host, port, path


def _normalize_path(path):
    # Collapse "." / ".." segments and duplicate slashes, the way a browser
    # resolves a relative link against the current page's directory.
    parts = []
    for seg in path.split("/"):
        if seg == "" or seg == ".":
            continue
        if seg == "..":
            if parts:
                parts.pop()
            continue
        parts.append(seg)
    return "/" + "/".join(parts)


def resolve(base_url, target):
    """Resolve a link/redirect target against the URL it was found on.

    Handles absolute URLs ("gemini://..."), protocol-relative
    ("//host/path"), root-relative ("/path"), and plain relative
    ("path", "../path") targets.
    """
    if "://" in target:
        return target
    if target.startswith("//"):
        return "gemini:" + target

    base_host, base_port, base_path = _parse_url(base_url)
    hostport = base_host if base_port == DEFAULT_PORT else "%s:%d" % (base_host, base_port)

    if target.startswith("/"):
        path = _normalize_path(target)
    else:
        base_dir = base_path.rsplit("/", 1)[0]
        path = _normalize_path(base_dir + "/" + target)

    return "gemini://%s%s" % (hostport, path)


def request(url, timeout=15):
    """Issue a single Gemini request and return a Response.

    Caller must close() the response (or use it as a context manager)
    once done with .content/.text -- the socket is kept open until then
    since Gemini bodies are EOF-terminated rather than length-prefixed.
    """
    host, port, path = _parse_url(url)

    # The request line is the full URL, not just the path, so one server
    # can host multiple virtual hosts.
    full_url = "gemini://%s%s" % (host, path)
    if len(full_url) > MAX_HEADER_LEN - 2:
        raise GeminiError("URL too long")

    ai = socket.getaddrinfo(host, port, 0, socket.SOCK_STREAM)[0]
    s = socket.socket(ai[0], socket.SOCK_STREAM, ai[2])

    if timeout is not None:
        # Must be set before wrap_socket() -- the returned SSLSocket
        # doesn't expose settimeout() of its own on this port.
        s.settimeout(timeout)

    try:
        s.connect(ai[-1])

        context = tls.SSLContext(tls.PROTOCOL_TLS_CLIENT)
        # TODO: Gemini's trust model is TOFU (trust-on-first-use) cert
        # pinning per host, not CA validation. Skip verification for now
        # and revisit once there's somewhere to persist known-host fingerprints.
        context.verify_mode = tls.CERT_NONE
        s = context.wrap_socket(s, server_hostname=host)

        s.write(full_url)
        s.write("\r\n")

        header = s.readline()
        if not header or not header.endswith(b"\r\n"):
            raise GeminiError("Malformed response header")

        header = str(header[:-2], "utf-8")
        if len(header) < 2 or not header[:2].isdigit():
            raise GeminiError("Malformed status code: %r" % header)

        status = int(header[:2])
        meta = header[3:] if len(header) > 3 else ""

        return Response(s, status, meta)
    except (OSError, GeminiError):
        s.close()
        raise


def get(url, timeout=15, max_redirects=5):
    """Fetch url, transparently following 3x redirects."""
    seen = set()
    for _ in range(max_redirects + 1):
        if url in seen:
            raise GeminiError("Redirect loop: " + url)
        seen.add(url)

        resp = request(url, timeout=timeout)
        if resp.category != 3:
            return resp

        target = resolve(url, resp.meta)
        resp.close()
        url = target

    raise GeminiError("Too many redirects")


def parse_gemtext(text):
    """Parse a text/gemini document into a list of (kind, text, url) lines.

    kind is one of: "text", "link", "h1", "h2", "h3", "list", "quote", "pre".
    url is only set for "link" lines. The ``` fence lines themselves are
    consumed (toggling preformatted mode) rather than emitted.
    """
    lines = []
    pre = False

    for raw in text.split("\n"):
        line = raw[:-1] if raw.endswith("\r") else raw

        if line.startswith("```"):
            pre = not pre
            continue

        if pre:
            lines.append(("pre", line, None))
        elif line.startswith("=>"):
            rest = line[2:].strip()
            if not rest:
                lines.append(("text", "", None))
                continue
            parts = rest.split(None, 1)
            url = parts[0]
            label = parts[1].strip() if len(parts) > 1 else url
            lines.append(("link", label, url))
        elif line.startswith("###"):
            lines.append(("h3", line[3:].strip(), None))
        elif line.startswith("##"):
            lines.append(("h2", line[2:].strip(), None))
        elif line.startswith("#"):
            lines.append(("h1", line[1:].strip(), None))
        elif line.startswith("* "):
            lines.append(("list", line[2:], None))
        elif line.startswith(">"):
            lines.append(("quote", line[1:].strip(), None))
        else:
            lines.append(("text", line, None))

    return lines


def _quote(s):
    """Percent-encode text for use as a Gemini input query string."""
    out = []
    for b in s.encode("utf-8"):
        c = chr(b)
        if c in _QUOTE_SAFE:
            out.append(c)
        else:
            out.append("%%%02X" % b)
    return "".join(out)


def _render_lines(parsed):
    """Turn parse_gemtext() output into (display_string, link_url) pairs."""
    out = []
    for kind, text, url in parsed:
        if kind == "h1":
            out.append((H1_FG + text + CLR, None))
        elif kind == "h2":
            out.append((H2_FG + text + CLR, None))
        elif kind == "h3":
            out.append((H3_FG + text + CLR, None))
        elif kind == "link":
            out.append((LINK_FG + "-> " + text + CLR, url))
        elif kind == "list":
            out.append(("  * " + text, None))
        elif kind == "quote":
            out.append((QUOTE_FG + "| " + text + CLR, None))
        elif kind == "pre":
            out.append((PRE_FG + text + CLR, None))
        else:
            out.append((text, None))
    return out


def _load(url):
    """Fetch url and return (status_kind, payload).

    status_kind is one of "page", "input", "error". payload is
    (display_lines, links) for "page", the input prompt for "input", or
    an error message for "error".
    """
    try:
        resp = get(url)
    except Exception as e:
        return "error", str(e)

    try:
        if resp.category == 1:
            return "input", resp.meta
        if resp.category == 2:
            mimetype = resp.meta.split(";", 1)[0].strip() or "text/gemini"
            body = resp.text
            if mimetype == "text/gemini":
                parsed = parse_gemtext(body)
            elif mimetype.startswith("text/"):
                parsed = [("text", line, None) for line in body.split("\n")]
            else:
                return "error", "Unsupported content type: " + mimetype

            rendered = _render_lines(parsed)
            links = {i: u for i, (_, u) in enumerate(rendered) if u}
            display = [s for s, _ in rendered]
            return "page", (display, links)
        return "error", "%d %s" % (resp.status, resp.meta)
    finally:
        resp.close()


def main(env, args):
    """ Creates a TUI for browsing gemini:// pages """

    url = args[0] if len(args) > 0 else None
    if not url:
        print("Usage: gemini <url>")
        return
    if "://" not in url:
        url = "gemini://" + url

    tui = env.tui
    tui.enter_altscreen()
    tui.cursor_hide()

    history = []
    display, links = [], {}
    prompt = ""
    error = ""
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

            kind, payload = _load(url)
            gc.collect()

            if kind == "page":
                display, links = payload
                ui_state = "PAGE"
            elif kind == "input":
                prompt = payload
                ui_state = "INPUT"
            else:
                error = payload
                ui_state = "ERROR"

        elif ui_state == "PAGE":
            tui.clear_screen()
            status = tui.make_label("[w/s] nav | [b]ack | [q]uit",
                                    0, env.rows-1,
                                    fg=0, bg=252,
                                    width=env.cols)

            lst = tui.make_list(display if display else ["(empty page)"],
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
                    target = links.get(lst.index)
                    if target:
                        history.append(url)
                        url = resolve(url, target)
                        ui_state = "LOAD"
                        break
                elif char == "w":
                    lst.up()
                elif char == "s":
                    lst.down()
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
                    url = url.split("?", 1)[0] + "?" + _quote(ans_input.value)
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
