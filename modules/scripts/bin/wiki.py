#
# MicroPython TUI Offline Wikipedia Reader
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import gc
import io
import struct
import sys

import deflate

DEFAULT_IDX_PATH = "/sd/wiki/simplewiki.idx"

FG = 252
BG = 18

LINK_COLOR = 45

CLR      = "\x1b[0m"
ERR_FG   = "\x1b[38;5;210m"

_MAGIC = b"VWIK"
_FORMAT_VERSION = 1
_HEADER_SIZE = 32

# Inline link markup delimiters -- see utils/wikiconvert.py's clean_wikitext()
# for how these get woven into an article's body on the desktop side.
_LINK_RS = "\x1e"
_LINK_US = "\x1f"

_MAX_SUGGESTIONS = 50


class WikiFormatError(Exception):
    pass


def _format_exception(e):
    """ str(exc) alone is often just "[Errno 22] EINVAL" -- useless for
    figuring out *where* things went wrong. Captures the full traceback
    (same as sys.print_exception() prints to the console) into a string
    so the ERROR state can show it directly in the app's own UI, since
    the T-Deck's screen may be the only console anyone's watching. """
    buf = io.StringIO()
    sys.print_exception(e, buf)
    return buf.getvalue()


class WikiHeader:
    __slots__ = (
        "article_count", "chunk_count", "title_record_size",
        "max_title_bytes", "chunk_table_offset", "title_index_offset",
    )

    def __init__(self, article_count, chunk_count, title_record_size,
                 max_title_bytes, chunk_table_offset, title_index_offset):
        self.article_count = article_count
        self.chunk_count = chunk_count
        self.title_record_size = title_record_size
        self.max_title_bytes = max_title_bytes
        self.chunk_table_offset = chunk_table_offset
        self.title_index_offset = title_index_offset


def normalize_title(title):
    """ Mirrors utils/wikiconvert.py's normalize_title() -- kept as a
    hand copy since there's no runtime that can import both a desktop
    and a MicroPython module. Any change there needs a matching change
    here, or on-device lookups stop matching the on-disk sort order. """
    if "#" in title:
        title = title[:title.index("#")]
    title = title.replace("_", " ")
    title = " ".join(title.split())
    if title:
        title = title[0].upper() + title[1:]
    return title


def _parse_header(buf):
    if len(buf) < _HEADER_SIZE:
        raise WikiFormatError("wiki index file is truncated")
    (magic, version, _flags, article_count, chunk_count,
     title_record_size, max_title_bytes,
     chunk_table_offset, title_index_offset) = struct.unpack("<4sHHIIHHII", buf[:28])

    if magic != _MAGIC:
        raise WikiFormatError("not a wiki index file")
    if version != _FORMAT_VERSION:
        raise WikiFormatError("unsupported wiki index format version %d" % version)

    return WikiHeader(article_count, chunk_count, title_record_size,
                       max_title_bytes, chunk_table_offset, title_index_offset)


class WikiIndex:
    """ Read-only access to a generated simplewiki.idx/.dat pair. The
    title index stays on the SD card and is binary-searched via seeks
    (it's tens of MB, too big to hold in RAM); the small chunk table
    is loaded once at open() time. """

    def __init__(self, idx_file, dat_file, header, chunk_table):
        self.idx_file = idx_file
        self.dat_file = dat_file
        self.header = header
        self.chunk_table = chunk_table

    @classmethod
    def open(cls, idx_path, dat_path):
        idx_file = open(idx_path, "rb")
        try:
            header = _parse_header(idx_file.read(_HEADER_SIZE))
            idx_file.seek(header.chunk_table_offset)
            raw = idx_file.read(header.chunk_count * 12)
            chunk_table = [
                struct.unpack("<III", raw[i:i + 12])
                for i in range(0, len(raw), 12)
            ]
        except Exception:
            idx_file.close()
            raise

        try:
            dat_file = open(dat_path, "rb")
        except OSError:
            idx_file.close()
            raise

        return cls(idx_file, dat_file, header, chunk_table)

    def close(self):
        try:
            self.idx_file.close()
        except OSError:
            pass
        try:
            self.dat_file.close()
        except OSError:
            pass

    def _title_bytes_at(self, i):
        self.idx_file.seek(self.header.title_index_offset + i * self.header.title_record_size)
        rec = self.idx_file.read(self.header.title_record_size)
        return rec[:self.header.max_title_bytes].rstrip(b"\x00"), rec

    def _lower_bound(self, target_bytes):
        """ First record index whose title is >= target_bytes (plain
        byte-wise compare, matching how the desktop script sorted
        NUL-padded fixed-width records). """
        lo, hi = 0, self.header.article_count
        while lo < hi:
            mid = (lo + hi) // 2
            rec_title, _ = self._title_bytes_at(mid)
            if rec_title < target_bytes:
                lo = mid + 1
            else:
                hi = mid
        return lo

    def find_exact(self, title):
        target = title.encode("utf-8")
        i = self._lower_bound(target)
        if i < self.header.article_count:
            rec_title, _ = self._title_bytes_at(i)
            if rec_title == target:
                return i
        return None

    def find_prefix(self, prefix, limit=_MAX_SUGGESTIONS):
        target = prefix.encode("utf-8")
        i = self._lower_bound(target)
        results = []
        while i < self.header.article_count and len(results) < limit:
            rec_title, _ = self._title_bytes_at(i)
            if not rec_title.startswith(target):
                break
            results.append((i, rec_title.decode("utf-8")))
            i += 1
        return results

    def _record_at(self, record_id):
        _, rec = self._title_bytes_at(record_id)
        title = rec[:self.header.max_title_bytes].rstrip(b"\x00").decode("utf-8")
        chunk_id, article_offset, article_len = struct.unpack(
            "<III", rec[self.header.max_title_bytes:self.header.max_title_bytes + 12]
        )
        return title, chunk_id, article_offset, article_len

    def load_article(self, record_id):
        """ Returns (title, body_bytes) for record_id -- decompresses
        that record's whole chunk (chunks are small, ~64KB decompressed
        by default) and slices out just this article's bytes. """
        title, chunk_id, article_offset, article_len = self._record_at(record_id)
        data_offset, compressed_len, decompressed_len = self.chunk_table[chunk_id]

        self.dat_file.seek(data_offset)
        compressed = self.dat_file.read(compressed_len)
        stream = deflate.DeflateIO(io.BytesIO(compressed), deflate.RAW, 15)
        decompressed = stream.read()
        if len(decompressed) != decompressed_len:
            raise WikiFormatError("chunk %d decompressed to the wrong size" % chunk_id)

        record_bytes = decompressed[article_offset:article_offset + article_len]
        link_count = struct.unpack("<H", record_bytes[:2])[0]
        body_offset = 2 + link_count * 4
        return title, record_bytes[body_offset:]


def _render_article(body_bytes):
    """ Splits an article body on its inline link markers into
    (text, target_record_id) segments -- link segments carry a record
    id, plain segments carry None. Fed straight into tui.make_pager(),
    which reflows every segment together into wrapped rows and owns
    link coloring itself, so no color codes or per-line splitting
    happens here -- embedded '\\n's (from headings) are handled by the
    pager's own row-breaking, not pre-split into separate entries. """
    text = body_bytes.decode("utf-8", "ignore")
    parts = text.split(_LINK_RS)
    segments = []
    for i, seg in enumerate(parts):
        if not seg:
            continue
        if i % 2 == 1:
            id_str, _, display = seg.partition(_LINK_US)
            try:
                target_id = int(id_str)
            except ValueError:
                target_id = None
            if target_id is not None:
                segments.append((display, target_id))
                continue
            seg = display  # malformed marker -- fall through, render as plain text
        segments.append((seg, None))
    return segments


def main(env, args):
    """ Creates a TUI for browsing an offline Simple English Wikipedia
    snapshot generated by utils/wikiconvert.py. """

    idx_path = args[0] if len(args) > 0 else DEFAULT_IDX_PATH
    dat_path = idx_path[:-4] + ".dat" if idx_path.endswith(".idx") else idx_path + ".dat"

    tui = env.tui
    tui.enter_altscreen()
    tui.cursor_hide()

    wiki = None
    error = ""

    if env.sd is None:
        error = "No SD card detected.\n\nInsert an SD card with wiki data and try again."
    else:
        try:
            wiki = WikiIndex.open(idx_path, dat_path)
        except OSError:
            error = ("No wiki data found at:\n%s\n\n"
                      "Generate it with utils/wikiconvert.py on a desktop\n"
                      "and copy the output to /sd/wiki/." % idx_path)
        except WikiFormatError as e:
            error = "Wiki data is unreadable:\n%s" % str(e)

    ui_state = "INPUT" if wiki else "ERROR"
    history = []
    segments = []
    suggestions = []
    title = ""

    while True:

        if ui_state == "INPUT":
            tui.clear_screen()
            bg_fill = tui.make_block("", 0, 0,
                                     width=env.cols, height=env.rows,
                                     fg=FG, bg=BG)
            bg_fill.draw()
            label = tui.make_label("Search Simple Wikipedia:",
                                   0, 1,
                                   fg=FG, bg=BG,
                                   align="center")

            query_input = tui.make_input("> ",
                                 0, 3,
                                 width=env.cols-2,
                                 fg=FG, bg=BG, input_bg=0,
                                 decorations=False,
                                 align="center")

            status = tui.make_label("[esc] %s | [enter] search" %
                                    ("quit" if not segments and not suggestions else "cancel"),
                                    0, env.rows-1,
                                    fg=0, bg=252,
                                    width=env.cols)

            tui.cursor_show()
            label.draw()
            status.draw()
            query_input.draw()

            while ui_state == "INPUT":
                char = sys.stdin.read(1)
                if char == "\n" or char == "\r":
                    if query_input.value:
                        tui.cursor_hide()
                        title = query_input.value
                        ui_state = "LOAD"
                        break
                elif char in ("\x08", "\x7f"):
                    query_input.backspace()
                elif char == "\x1b":
                    tui.cursor_hide()
                    ui_state = "PAGE" if segments else "SUGGEST" if suggestions else "QUIT"
                    break
                else:
                    query_input.push(char)
                query_input.draw()
                tui.draw()

        elif ui_state == "LOAD":
            tui.clear_screen()
            bg_fill = tui.make_block("", 0, 0,
                                     width=env.cols, height=env.rows,
                                     fg=FG, bg=BG)
            bg_fill.draw()
            tui.draw_label("Searching for '%s' ..." % title,
                           0, env.rows // 2,
                           fg=FG, bg=BG,
                           align="center")
            tui.draw()

            try:
                norm = normalize_title(title)
                record_id = wiki.find_exact(norm)
                if record_id is not None:
                    title, body = wiki.load_article(record_id)
                    segments = _render_article(body)
                    suggestions = []
                    ui_state = "PAGE"
                else:
                    matches = wiki.find_prefix(norm)
                    segments = []
                    if matches:
                        suggestions = matches
                        ui_state = "SUGGEST"
                    else:
                        error = "No article found for '%s'." % title
                        ui_state = "ERROR"
            except (OSError, WikiFormatError) as e:
                error = _format_exception(e)
                ui_state = "ERROR"
            gc.collect()

        elif ui_state == "PAGE":
            tui.clear_screen()
            # "n"/"N" are taken by link navigation here (unlike dict.py's
            # PAGE state), so "new search" moves to "/" -- same key `less`
            # itself uses to start a search, which fits the pager framing.
            status = tui.make_label("[w/s] nav | [n/N] links | [x] search",
                                    0, env.rows-1,
                                    fg=0, bg=252,
                                    width=env.cols)

            pager = tui.make_pager(segments if segments else [("(empty article)", None)],
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
                        history.append(title)
                        try:
                            title, body = wiki.load_article(target)
                            segments = _render_article(body)
                            gc.collect()
                        except (OSError, WikiFormatError) as e:
                            error = _format_exception(e)
                            ui_state = "ERROR"
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
                        target_title = history.pop()
                        record_id = wiki.find_exact(normalize_title(target_title))
                        if record_id is not None:
                            title, body = wiki.load_article(record_id)
                            segments = _render_article(body)
                            gc.collect()
                        break
                elif char == "x":
                    ui_state = "INPUT"
                    break
                elif char == "q":
                    ui_state = "QUIT"
                    break

        elif ui_state == "SUGGEST":
            tui.clear_screen()
            status = tui.make_label("[w/s] nav | [enter] open | [n]ew search | [q]uit",
                                    0, env.rows-1,
                                    fg=0, bg=252,
                                    width=env.cols)

            lines = [t for _, t in suggestions]

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
                    record_id, _ = suggestions[lst.index]
                    try:
                        title, body = wiki.load_article(record_id)
                        segments = _render_article(body)
                        suggestions = []
                        ui_state = "PAGE"
                    except (OSError, WikiFormatError) as e:
                        error = _format_exception(e)
                        ui_state = "ERROR"
                    gc.collect()
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

            can_search = wiki is not None
            status_text = "[n]ew search | [q]uit" if can_search else "[q]uit"
            status = tui.make_label(status_text,
                                    0, env.rows-1,
                                    fg=0, bg=252,
                                    width=env.cols)

            while True:
                blk.draw()
                status.draw()
                tui.draw()

                char = sys.stdin.read(1)
                if char == "n" and can_search:
                    ui_state = "INPUT"
                    break
                elif char == "q":
                    ui_state = "QUIT"
                    break

        elif ui_state == "QUIT":
            if wiki:
                wiki.close()
            gc.collect()
            tui.exit_altscreen()
            tui.cursor_show()
            return
