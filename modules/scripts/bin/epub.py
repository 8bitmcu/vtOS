#
# MicroPython TUI EPUB Reader
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import gc
import io
import sys

import epubfmt
import epubzip

FG = 252
BG = 18

LINK_COLOR = 45

CLR    = "\x1b[0m"
ERR_FG = "\x1b[38;5;210m"


def _format_exception(e):
    """ str(exc) alone is often just "[Errno 22] EINVAL" -- useless for
    figuring out *where* things went wrong. Same approach as
    bin/wiki.py's _format_exception(). """
    buf = io.StringIO()
    sys.print_exception(e, buf)
    return buf.getvalue()


def main(env, args):
    """ Creates a TUI for reading an EPUB file: a table-of-contents list
    screen backed by epubfmt.EpubBook, and a chapter pager screen reusing
    tui.make_pager() the same way bin/wiki.py does. """

    path = args[0] if len(args) > 0 else None
    if not path:
        print("Usage: epub <path.epub>")
        return

    tui = env.tui
    tui.enter_altscreen()
    tui.cursor_hide()

    book = None
    error = ""
    try:
        book = epubfmt.EpubBook(path)
    except (OSError, epubzip.EpubZipError, epubfmt.EpubFormatError) as e:
        error = _format_exception(e)

    toc = []
    chapters = []
    if book is not None:
        chapters = book.chapter_hrefs()
        toc = book.toc if book.toc else [
            ("Chapter %d" % (i + 1), href) for i, href in enumerate(chapters)
        ]

    ui_state = "TOC" if book is not None else "ERROR"
    history = []       # stack of previously-visited hrefs, for [b]ack
    segments = []
    href = None         # href of the chapter currently shown in PAGE

    def _load(target_href):
        nonlocal href, segments
        href = target_href
        segments = book.read_chapter_segments(href)
        gc.collect()

    while True:

        if ui_state == "TOC":
            tui.clear_screen()
            win = tui.make_window(
                    0, 0,
                    width=env.cols, height=env.rows,
                    title=book.title,
                    fg=FG, bg=BG)

            status = win.make_label("[w/s] nav | [enter] open | [q]uit",
                                    0, win.inner_h - 1,
                                    fg=0, bg=252,
                                    width=win.inner_w)

            lines = [t for t, _ in toc]
            lst = win.make_list(lines,
                                x=0, y=0,
                                width=win.inner_w, height=win.inner_h - 1,
                                fg=FG, bg=BG,
                                arrow=">", left_pad=1,
                                multiline=True, wrap=True)

            win.invalidate()
            while True:
                win.draw()
                lst.draw()
                status.draw()
                tui.draw()

                char = sys.stdin.read(1)
                if char == "\n" or char == "\r":
                    _, target_href = toc[lst.index]
                    try:
                        _load(target_href)
                        history = []
                        ui_state = "PAGE"
                    except (OSError, epubzip.EpubZipError) as e:
                        error = _format_exception(e)
                        ui_state = "ERROR"
                    break
                elif char == "w":
                    lst.up()
                elif char == "s":
                    lst.down()
                elif char == "q":
                    ui_state = "QUIT"
                    break

        elif ui_state == "PAGE":
            tui.clear_screen()
            status = tui.make_label(
                    "[w/s] nav | [c/C] chptr | [t]oc | [q]uit",
                    0, env.rows - 1,
                    fg=0, bg=252,
                    width=env.cols)

            pager = tui.make_pager(segments if segments else [("(empty chapter)", None)],
                                   0, 0,
                                   width=env.cols, height=env.rows - 1,
                                   fg=FG, bg=BG,
                                   link_fg=LINK_COLOR, link_bg=BG,
                                   cur_fg=BG, cur_bg=LINK_COLOR)

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
                elif char == "C" or char == "c":
                    if href in chapters:
                        idx = chapters.index(href) + (1 if char == "c" else -1)
                        if 0 <= idx < len(chapters):
                            try:
                                history.append(href)
                                _load(chapters[idx])
                            except (OSError, epubzip.EpubZipError) as e:
                                error = _format_exception(e)
                                ui_state = "ERROR"
                            break
                elif char == "t":
                    ui_state = "TOC"
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
            if book is not None:
                book.close()
            gc.collect()
            tui.exit_altscreen()
            tui.cursor_show()
            return
