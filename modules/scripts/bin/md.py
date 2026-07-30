#
# MicroPython TUI Markdown / Plain Text Viewer
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import gc
import io
import sys

FG = 252
BG = 18

CLR      = "\x1b[0m"
BOLD     = "\x1b[1m"
ITALIC   = "\x1b[3m"
ERR_FG   = "\x1b[38;5;210m"
LINK_FG  = "\x1b[38;5;45m"
QUOTE_FG = "\x1b[38;5;244m"
PRE_FG   = "\x1b[38;5;244m"
CODE_FG  = "\x1b[38;5;244m"

# H1/H2/H3 match the palette bin/gemini.py already established; H4-H6
# just reuse H3's weight -- most real documents don't nest that deep,
# and it's not worth inventing three more colors to distinguish them.
_HEADING_FG = [
    "\x1b[1;38;5;255m",
    "\x1b[1;38;5;250m",
    "\x1b[38;5;250m",
    "\x1b[38;5;250m",
    "\x1b[38;5;250m",
    "\x1b[38;5;250m",
]


def _format_exception(e):
    """ Same approach as bin/wiki.py's _format_exception(). """
    buf = io.StringIO()
    sys.print_exception(e, buf)
    return buf.getvalue()


def _inline(text):
    """ Renders markdown inline syntax (bold, italic, inline code, links,
    images) into ANSI-styled text via manual character scanning --
    MicroPython's `re` module doesn't reliably support the lazy
    quantifiers a regex-based approach would need. Links render as
    colored text only (no navigation -- this is a viewer, not a
    browser); images fall back to the same "[image: alt]" placeholder
    bin/epub.py uses. """
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]

        if text.startswith("**", i) or text.startswith("__", i):
            marker = text[i:i + 2]
            end = text.find(marker, i + 2)
            if end != -1:
                out.append(BOLD + text[i + 2:end] + CLR)
                i = end + 2
                continue

        if c in ("*", "_"):
            end = text.find(c, i + 1)
            if end != -1 and end > i + 1:
                out.append(ITALIC + text[i + 1:end] + CLR)
                i = end + 1
                continue

        if c == "`":
            end = text.find("`", i + 1)
            if end != -1:
                out.append(CODE_FG + text[i + 1:end] + CLR)
                i = end + 1
                continue

        if c == "!" and i + 1 < n and text[i + 1] == "[":
            close = text.find("]", i + 2)
            if close != -1 and close + 1 < n and text[close + 1] == "(":
                paren_end = text.find(")", close + 2)
                if paren_end != -1:
                    alt = text[i + 2:close].strip()
                    out.append("[image: %s]" % alt if alt else "[image]")
                    i = paren_end + 1
                    continue

        if c == "[":
            close = text.find("]", i + 1)
            if close != -1 and close + 1 < n and text[close + 1] == "(":
                paren_end = text.find(")", close + 2)
                if paren_end != -1:
                    label = text[i + 1:close]
                    out.append(LINK_FG + label + CLR)
                    i = paren_end + 1
                    continue

        out.append(c)
        i += 1

    return "".join(out)


def _is_hr(stripped):
    compact = stripped.replace(" ", "")
    return len(compact) >= 3 and compact[0] in "-*_" and all(ch == compact[0] for ch in compact)


def _match_bullet(line):
    i = 0
    while i < len(line) and line[i] == " ":
        i += 1
    if i < len(line) and line[i] in "-*+" and i + 1 < len(line) and line[i + 1] == " ":
        return i // 2, line[i + 2:]
    return None


def _match_ordered(line):
    i = 0
    while i < len(line) and line[i] == " ":
        i += 1
    j = i
    while j < len(line) and line[j].isdigit():
        j += 1
    if j > i and j + 1 < len(line) and line[j] == "." and line[j + 1] == " ":
        return i // 2, line[i:j + 1], line[j + 2:]
    return None


def _render_markdown(text):
    """ Splits markdown into (text, None) segments for tui.make_pager() --
    same convention bin/wiki.py/bin/epub.py use (link segments would
    carry a target, but this viewer never sets one). Each source line
    becomes its own segment -- single newlines are preserved as real
    line breaks rather than joined into a flowing paragraph, since a
    lot of the plain text this views (see sys/defaults.py's WELCOME.md)
    uses single newlines as meaningful line breaks, not editor-width
    wrapping. Runs of 2+ blank lines collapse down to one, so stray
    extra blank lines in the source don't pile up as vertical gaps. """
    lines = text.replace("\r\n", "\n").split("\n")
    segments = []
    in_code = False
    last_blank = True  # seed True so a leading blank line is dropped, not kept

    for line in lines:
        stripped = line.strip()

        if stripped.startswith("```") or stripped.startswith("~~~"):
            in_code = not in_code
            last_blank = False
            continue

        if in_code:
            segments.append((PRE_FG + line + CLR + "\n", None))
            last_blank = False
            continue

        if not stripped:
            if not last_blank:
                segments.append(("\n", None))
            last_blank = True
            continue
        last_blank = False

        if stripped.startswith("#"):
            level = 0
            while level < len(stripped) and stripped[level] == "#":
                level += 1
            if 1 <= level <= 6 and level < len(stripped) and stripped[level] == " ":
                heading_text = stripped[level:].strip()
                fg = _HEADING_FG[level - 1]
                segments.append((fg + _inline(heading_text) + CLR + "\n", None))
                continue

        if _is_hr(stripped):
            segments.append(("-" * 40 + "\n", None))
            continue

        if stripped.startswith(">"):
            quoted = stripped[1:].strip()
            segments.append((QUOTE_FG + "| " + _inline(quoted) + CLR + "\n", None))
            continue

        bullet = _match_bullet(line)
        if bullet is not None:
            indent, content = bullet
            segments.append(("  " * indent + "- " + _inline(content) + "\n", None))
            continue

        ordered = _match_ordered(line)
        if ordered is not None:
            indent, marker, content = ordered
            segments.append(("  " * indent + marker + " " + _inline(content) + "\n", None))
            continue

        segments.append((_inline(stripped) + "\n", None))

    return segments


def main(env, args):
    """ Creates a TUI for reading a local Markdown (.md/.markdown) or
    plain text file, rendered through tui.make_pager() -- the same
    widget bin/wiki.py, bin/gemini.py, bin/gopher.py and bin/epub.py
    use. """

    path = args[0] if len(args) > 0 else None
    if not path:
        print("Usage: md <path.md|path.txt>")
        return

    tui = env.tui
    tui.enter_altscreen()
    tui.cursor_hide()

    segments = []
    error = ""
    try:
        with open(path) as f:
            text = f.read()
        is_markdown = path.endswith(".md") or path.endswith(".markdown")
        segments = _render_markdown(text) if is_markdown else [(text, None)]
        gc.collect()
    except OSError as e:
        error = _format_exception(e)

    ui_state = "PAGE" if not error else "ERROR"

    while True:

        if ui_state == "PAGE":
            tui.clear_screen()
            status = tui.make_label("[w/s] line | [W/S] page | [q]uit",
                                    0, env.rows - 1,
                                    fg=0, bg=252,
                                    width=env.cols)

            pager = tui.make_pager(segments if segments else [("(empty file)", None)],
                                   0, 0,
                                   width=env.cols, height=env.rows - 1,
                                   fg=FG, bg=BG)

            while True:
                pager.draw()
                status.draw()
                tui.draw()

                char = sys.stdin.read(1)
                if char == "w":
                    pager.up()
                elif char == "s":
                    pager.down()
                elif char == "W":
                    pager.page_up()
                elif char == "S":
                    pager.page_down()
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
            gc.collect()
            tui.exit_altscreen()
            tui.cursor_show()
            return
