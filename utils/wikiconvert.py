#!/usr/bin/env python3

# This utility converts a Simple English Wikipedia XML dump into the
# compact binary format vtOS's `wiki` command reads from /sd/wiki/.
#
# Pipeline: download the dump (or use a local file), stream-parse the
# MediaWiki XML in two passes (pass 1 collects titles/redirects, pass 2
# cleans wikitext and emits compressed article chunks), and write out
# an index + data file pair sized to fit comfortably under ~1GB.
#
# Requires: mwparserfromhell, requests (see utils/.venv for a pinned
# environment: `uv venv utils/.venv && uv pip install --python
# utils/.venv/bin/python mwparserfromhell pytest requests`).

import argparse
import bz2
import json
import os
import re
import struct
import sys
import time
import xml.etree.ElementTree as ET
import zlib

import mwparserfromhell
import requests

DEFAULT_DUMP_URL = "https://dumps.wikimedia.org/simplewiki/latest/simplewiki-latest-pages-articles.xml.bz2"

# Wikimedia's dump server rejects requests with a generic/default client
# User-Agent (like requests' own "python-requests/x.y") with a 403 --
# see https://meta.wikimedia.org/wiki/User-Agent_policy. A browser sends
# its own descriptive UA and isn't affected, which is why this can look
# like it "only fails from a script."
_DOWNLOAD_HEADERS = {
    "User-Agent": "vtOS-wikiconvert/1 (https://github.com/8bitmcu/vtOS; "
                  "offline Wikipedia reader data-prep script)",
}

MAGIC = b"VWIK"
FORMAT_VERSION = 1

# Header: magic, version, flags, article_count, chunk_count,
# title_record_size, max_title_bytes, chunk_table_offset,
# title_index_offset, 4 bytes reserved padding. 32 bytes total.
_HEADER_STRUCT = struct.Struct("<4sHHIIHHII4x")

# Chunk table entry: (data_offset_in_dat, compressed_len, decompressed_len).
_CHUNK_ENTRY_STRUCT = struct.Struct("<III")

# Article record trailer fields packed after the variable-length link
# id list: none -- link_count (H) and each link id (I) precede the body.
_LINK_COUNT_STRUCT = struct.Struct("<H")
_LINK_ID_STRUCT = struct.Struct("<I")


class IndexHeader:
    __slots__ = (
        "magic", "version", "flags", "article_count", "chunk_count",
        "title_record_size", "max_title_bytes",
        "chunk_table_offset", "title_index_offset",
    )

    def __init__(self, magic, version, flags, article_count, chunk_count,
                 title_record_size, max_title_bytes,
                 chunk_table_offset, title_index_offset):
        self.magic = magic
        self.version = version
        self.flags = flags
        self.article_count = article_count
        self.chunk_count = chunk_count
        self.title_record_size = title_record_size
        self.max_title_bytes = max_title_bytes
        self.chunk_table_offset = chunk_table_offset
        self.title_index_offset = title_index_offset


def normalize_title(title):
    """ Normalizes a wikilink target/page title the same way MediaWiki
    does for comparison purposes: strip any '#section' anchor, turn
    '_' into spaces (both are valid separators in wikitext), collapse
    whitespace, and uppercase only the first character (MediaWiki
    title case only affects the first letter, not the whole string).

    This is deliberately hand-mirrored on-device (modules/scripts/bin/
    wiki.py) rather than shared via import -- there's no runtime that
    can import both a desktop and a MicroPython module. Keep the two
    in sync if this changes. """
    title = title.split("#", 1)[0]
    title = title.replace("_", " ")
    title = re.sub(r"\s+", " ", title).strip()
    if title:
        title = title[0].upper() + title[1:]
    return title


def build_index_header(article_count, chunk_count, title_record_size,
                        max_title_bytes, chunk_table_offset, title_index_offset):
    return _HEADER_STRUCT.pack(
        MAGIC, FORMAT_VERSION, 0,
        article_count, chunk_count,
        title_record_size, max_title_bytes,
        chunk_table_offset, title_index_offset,
    )


def parse_index_header(buf):
    (magic, version, flags, article_count, chunk_count,
     title_record_size, max_title_bytes,
     chunk_table_offset, title_index_offset) = _HEADER_STRUCT.unpack(buf[:_HEADER_STRUCT.size])

    if magic != MAGIC:
        raise ValueError("not a wiki index file (bad magic %r)" % (magic,))
    if version != FORMAT_VERSION:
        raise ValueError("unsupported wiki index format version %d" % version)

    return IndexHeader(
        magic, version, flags, article_count, chunk_count,
        title_record_size, max_title_bytes,
        chunk_table_offset, title_index_offset,
    )


def pack_chunk_entry(data_offset, compressed_len, decompressed_len):
    return _CHUNK_ENTRY_STRUCT.pack(data_offset, compressed_len, decompressed_len)


def unpack_chunk_entry(buf):
    return _CHUNK_ENTRY_STRUCT.unpack(buf[:_CHUNK_ENTRY_STRUCT.size])


def pack_title_record(title, chunk_id, article_offset, article_len, max_title_bytes):
    title_bytes = title.encode("utf-8")
    if len(title_bytes) > max_title_bytes:
        raise ValueError(
            "title %r is %d bytes, exceeds max_title_bytes=%d"
            % (title, len(title_bytes), max_title_bytes)
        )
    title_field = title_bytes.ljust(max_title_bytes, b"\x00")
    return title_field + struct.pack("<III", chunk_id, article_offset, article_len)


def unpack_title_record(buf, max_title_bytes):
    title_field = buf[:max_title_bytes]
    chunk_id, article_offset, article_len = struct.unpack(
        "<III", buf[max_title_bytes:max_title_bytes + 12]
    )
    title = title_field.rstrip(b"\x00").decode("utf-8")
    return title, chunk_id, article_offset, article_len


def pack_article_record(link_ids, body):
    parts = [_LINK_COUNT_STRUCT.pack(len(link_ids))]
    for link_id in link_ids:
        parts.append(_LINK_ID_STRUCT.pack(link_id))
    parts.append(body)
    return b"".join(parts)


def unpack_article_record(buf):
    (link_count,) = _LINK_COUNT_STRUCT.unpack_from(buf, 0)
    offset = _LINK_COUNT_STRUCT.size
    link_ids = []
    for _ in range(link_count):
        (link_id,) = _LINK_ID_STRUCT.unpack_from(buf, offset)
        link_ids.append(link_id)
        offset += _LINK_ID_STRUCT.size
    body = buf[offset:]
    return link_ids, body


# ---------------------------------------------------------------------------
# Page filtering
# ---------------------------------------------------------------------------

_ARTICLE_NAMESPACE = "0"

_REDIRECT_RE = re.compile(
    r"^\s*#REDIRECT\s*:?\s*\[\[\s*([^\]|#]+)", re.IGNORECASE
)

_DISAMBIG_RE = re.compile(
    r"\{\{\s*disambig|\{\{\s*disambiguation\b|category:\s*disambiguation",
    re.IGNORECASE,
)


def is_article_namespace(ns):
    """ Simple English Wikipedia dumps mark every <page>'s namespace
    with a <ns> child holding the namespace id as a string ("0" for
    real articles; "14" Category, "6" File, etc, and named
    pseudo-namespaces like "Talk" show up unparsed straight from a
    <title> prefix in some callers). Only ns "0" is kept. """
    return ns == "0"


def parse_redirect_target(wikitext):
    """ Returns the normalized target title if wikitext is a redirect
    page (starts with '#REDIRECT [[Target]]', optionally piped), else
    None. This is the dump-format-independent signal -- the XML
    dump's own <redirect title="..."> attribute says the same thing,
    but re-deriving it from the wikitext body means this function only
    needs the page text, not the surrounding XML element. """
    match = _REDIRECT_RE.match(wikitext)
    if not match:
        return None
    return normalize_title(match.group(1))


def is_disambiguation(wikitext):
    """ Heuristic: Simple Wikipedia disambiguation pages carry a
    {{disambig}}/{{disambiguation}} template or a Disambiguation
    pages category link. Good enough to filter the bulk of them;
    false negatives just leave a low-value stub article in, which is
    harmless. """
    return _DISAMBIG_RE.search(wikitext) is not None


# ---------------------------------------------------------------------------
# Chunking
# ---------------------------------------------------------------------------

def chunk_articles(records, chunk_size):
    """ Groups an iterable of (key, packed_record_bytes) pairs into
    concatenated chunks no larger than chunk_size, without ever
    splitting a single record across chunks (a record bigger than
    chunk_size on its own just becomes an oversized chunk of one).

    Yields (chunk_bytes, placements) where placements is a list of
    (key, offset_within_chunk, length) for every record folded into
    that chunk, in order. """
    buffer = bytearray()
    placements = []

    for key, data in records:
        if buffer and len(buffer) + len(data) > chunk_size:
            yield bytes(buffer), placements
            buffer = bytearray()
            placements = []

        offset = len(buffer)
        buffer += data
        placements.append((key, offset, len(data)))

    if placements:
        yield bytes(buffer), placements


# ---------------------------------------------------------------------------
# Wikitext cleaning
# ---------------------------------------------------------------------------

# Inline link markup delimiters -- see the format note on pack_article_record
# for the on-disk side of this. Chosen as C0 control bytes that never occur
# in normal prose, so the on-device renderer can split on them with no
# regex/markup parser at all.
_LINK_RS = "\x1e"   # segment delimiter: alternates plain/link segments
_LINK_US = "\x1f"   # within a link segment, separates record id from display text

_DROPPED_TAGS = frozenset(("ref", "table", "gallery", "references"))
_DROPPED_LINK_NAMESPACES = frozenset(("file", "image", "category"))

# Any stray C0 control byte that could be confused with our own markers
# (or is otherwise not printable prose) gets stripped from source text
# before it reaches the output -- see pack_article_record's format note.
_STRAY_CONTROL_RE = re.compile("[\x00-\x1f\x7f]")

_WHITESPACE_RUN_RE = re.compile(r"[ \t]+")
_BLANK_LINE_RUN_RE = re.compile(r"\n{3,}")


def _strip_wiki_tables(text):
    """ mwparserfromhell doesn't model MediaWiki's pipe-table syntax
    ("{| ... |}") as a distinct node -- it's line-oriented markup, not
    template/tag syntax -- so a naive parse leaves raw pipes and cell
    markers in the output. Strip balanced {| ... |} blocks up front,
    with a small depth counter since tables can nest (rare, but seen
    on Simple Wikipedia's more elaborate infobox-adjacent pages). """
    out = []
    depth = 0
    i = 0
    n = len(text)
    while i < n:
        if text[i:i + 2] == "{|":
            depth += 1
            i += 2
            continue
        if text[i:i + 2] == "|}" and depth > 0:
            depth -= 1
            i += 2
            continue
        if depth == 0:
            out.append(text[i])
        i += 1
    return "".join(out)


def _resolve_link_target(raw_title, title_to_record_id, redirect_map, max_redirect_depth):
    """ Normalizes raw_title and looks it up in the kept-article set,
    following redirect_map chains (with a depth cap and a cycle
    guard) when the direct title isn't itself a kept article. Returns
    a record id, or None if the link is dead (or a runaway/cyclic
    redirect chain). """
    title = normalize_title(raw_title)
    seen = set()
    for _ in range(max_redirect_depth + 1):
        record_id = title_to_record_id.get(title)
        if record_id is not None:
            return record_id
        if title in seen:
            return None
        seen.add(title)
        next_title = redirect_map.get(title)
        if next_title is None:
            return None
        title = next_title
    return None


def _clean_text(raw):
    return _STRAY_CONTROL_RE.sub("", raw)


def _walk_nodes(nodes, title_to_record_id, redirect_map, max_redirect_depth, link_ids, out):
    for node in nodes:
        kind = type(node).__name__

        if kind == "Text":
            out.append(_clean_text(str(node)))

        elif kind == "Wikilink":
            target_raw = str(node.title).strip()
            namespace = target_raw.split(":", 1)[0].strip().lower() if ":" in target_raw else ""
            if namespace in _DROPPED_LINK_NAMESPACES:
                continue  # file/image/category links (and any caption text) are dropped whole

            display = _clean_text(str(node.text).strip() if node.text is not None else target_raw)
            record_id = _resolve_link_target(
                target_raw, title_to_record_id, redirect_map, max_redirect_depth
            )
            if record_id is None:
                out.append(display)
            else:
                link_ids.append(record_id)
                out.append(_LINK_RS + str(record_id) + _LINK_US + display + _LINK_RS)

        elif kind == "ExternalLink":
            if node.title is not None:
                out.append(_clean_text(str(node.title).strip()))
            # else: a bare URL with no display text -- dead weight offline, drop it

        elif kind == "HTMLEntity":
            try:
                out.append(node.normalize())
            except ValueError:
                pass

        elif kind == "Heading":
            _walk_nodes(node.title.nodes, title_to_record_id, redirect_map,
                        max_redirect_depth, link_ids, out)
            out.append("\n")

        elif kind == "Tag":
            if str(node.tag) not in _DROPPED_TAGS:
                _walk_nodes(node.contents.nodes, title_to_record_id, redirect_map,
                            max_redirect_depth, link_ids, out)

        # Template, Comment, Argument, and dropped Tag nodes are silently
        # skipped -- a TUI can't usefully render any of them.


def clean_wikitext(wikitext, title_to_record_id, redirect_map, max_redirect_depth=5):
    """ Converts raw wikitext into (body_bytes, link_record_ids):
    plain readable text with inline link markers (see _LINK_RS/_LINK_US
    above), and the list of record ids referenced by those markers, in
    the order they appear.

    title_to_record_id maps normalized kept-article titles to their
    record id; redirect_map maps normalized redirect-page titles to
    their normalized target title (possibly another redirect, up to
    max_redirect_depth hops). Links that don't resolve to a kept
    article render as plain display text with no markers. """
    wikitext = _strip_wiki_tables(wikitext)
    code = mwparserfromhell.parse(wikitext)

    for template in code.filter_templates(recursive=True):
        try:
            code.remove(template)
        except ValueError:
            pass
    for comment in code.filter_comments(recursive=True):
        try:
            code.remove(comment)
        except ValueError:
            pass

    link_ids = []
    out = []
    _walk_nodes(code.nodes, title_to_record_id, redirect_map, max_redirect_depth, link_ids, out)

    text = "".join(out)
    text = _WHITESPACE_RUN_RE.sub(" ", text)
    text = _BLANK_LINE_RUN_RE.sub("\n\n", text)
    # Plain str.strip() also treats \x1c-\x1f as whitespace (their Unicode
    # bidirectional class is "space"-like) and would eat a leading/trailing
    # _LINK_RS marker whenever an article starts or ends with a link --
    # strip only real whitespace explicitly instead.
    text = text.strip(" \t\r\n")

    return text.encode("utf-8"), link_ids


# ---------------------------------------------------------------------------
# Dump download (I/O glue -- not unit tested)
# ---------------------------------------------------------------------------

def _format_progress(written, total, width=30):
    if total:
        frac = min(written / total, 1.0)
        filled = int(frac * width)
        bar = "#" * filled + "-" * (width - filled)
        return f"[{bar}] {frac * 100:5.1f}% ({written / 1e6:.0f}MB/{total / 1e6:.0f}MB)"
    return f"{written / 1e6:.0f}MB downloaded (server didn't report a total size)"


def download_dump(url, dest_path, force=False):
    """ Downloads url to dest_path, streaming to avoid holding the
    whole (multi-hundred-MB) dump in memory. Skips the download if
    dest_path already exists and looks complete, unless force=True --
    re-running the conversion while iterating on parsing logic
    shouldn't have to re-fetch the dump every time.

    Progress prints are explicitly flushed: stdout is fully buffered
    (not line-buffered) whenever it isn't a real TTY -- which is the
    case for `docker run` without -t -- so without flush=True these
    would all queue up silently and the download would look hung
    until the buffer happened to fill or the process exited. """
    if os.path.exists(dest_path) and not force:
        print(f"wikiconvert: using cached dump at {dest_path} (pass --force-download to refetch)",
              flush=True)
        return dest_path

    tmp_path = dest_path + ".part"
    print(f"wikiconvert: downloading {url}", flush=True)
    with requests.get(url, stream=True, timeout=60, headers=_DOWNLOAD_HEADERS) as resp:
        resp.raise_for_status()
        total = int(resp.headers.get("content-length", 0))
        written = 0
        last_report = time.monotonic()
        with open(tmp_path, "wb") as f:
            for chunk in resp.iter_content(chunk_size=1024 * 1024):
                if not chunk:
                    continue
                f.write(chunk)
                written += len(chunk)
                now = time.monotonic()
                if now - last_report > 0.5:
                    print(f"\rwikiconvert: {_format_progress(written, total)}", end="", flush=True)
                    last_report = now
        print(f"\rwikiconvert: {_format_progress(written, total)}", flush=True)
    os.replace(tmp_path, dest_path)
    return dest_path


# ---------------------------------------------------------------------------
# MediaWiki XML dump streaming parse
# ---------------------------------------------------------------------------

def _localname(tag):
    """ ElementTree qualifies tags with the MediaWiki export XML
    namespace URI, e.g. "{http://www.mediawiki.org/xml/export-0.10/}page"
    -- strip that prefix so callers can match on plain tag names
    without hardcoding a specific export schema version. """
    return tag.rsplit("}", 1)[-1] if "}" in tag else tag


class DumpPage:
    __slots__ = ("title", "ns", "redirect_title", "text")

    def __init__(self, title, ns, redirect_title, text):
        self.title = title
        self.ns = ns
        self.redirect_title = redirect_title
        self.text = text


def iterate_dump_pages(dump_path):
    """ Streams <page> elements out of a bz2-compressed MediaWiki
    export XML dump, one DumpPage at a time, clearing each element
    after it's read so the parse doesn't hold the full (multi-GB
    decompressed) document in memory. """
    with bz2.BZ2File(dump_path, "rb") as f:
        for event, elem in ET.iterparse(f, events=("end",)):
            if _localname(elem.tag) != "page":
                continue

            title = ""
            ns = ""
            redirect_title = None
            text = ""

            for child in elem:
                ctag = _localname(child.tag)
                if ctag == "title":
                    title = child.text or ""
                elif ctag == "ns":
                    ns = child.text or ""
                elif ctag == "redirect":
                    redirect_title = child.get("title")
                elif ctag == "revision":
                    for rchild in child:
                        if _localname(rchild.tag) == "text":
                            text = rchild.text or ""

            yield DumpPage(title, ns, redirect_title, text)
            elem.clear()


# ---------------------------------------------------------------------------
# Two-pass conversion driver
# ---------------------------------------------------------------------------

def _is_kept_article(page, min_article_chars):
    """ Shared keep/drop decision used identically by both passes --
    pass 1 (to decide the sorted title set) and pass 2 (to decide
    which pages to actually clean+emit) must agree, or record ids
    assigned in pass 1 won't line up with the articles pass 2 emits. """
    if not is_article_namespace(page.ns):
        return False
    if page.redirect_title is not None:
        return False
    if parse_redirect_target(page.text) is not None:
        return False
    if is_disambiguation(page.text):
        return False
    if len(page.text.strip()) < min_article_chars:
        return False
    return True


def _collect_titles_and_redirects(dump_path, min_article_chars, max_title_bytes):
    """ Pass 1: decides the final kept-article set and assigns each a
    stable record id (= its position in normalized-title sort order),
    and separately collects every redirect's (source -> target)
    mapping regardless of whether the target ends up kept (a dangling
    redirect just means links through it stay dead links).

    A title that doesn't fit in max_title_bytes is skipped (with a
    warning) rather than failing the whole conversion -- real Wikipedia
    has a handful of legitimately absurd titles (e.g. herbarium
    specimen labels used as article titles) far longer than any
    reasonable fixed-width record size; losing a few of those articles
    beats blocking every run over an outlier. """
    kept_titles = set()
    redirect_map = {}
    skipped_long_titles = 0

    for page in iterate_dump_pages(dump_path):
        norm_title = normalize_title(page.title)

        if page.redirect_title is not None:
            redirect_map[norm_title] = normalize_title(page.redirect_title)
            continue

        target = parse_redirect_target(page.text)
        if target is not None:
            redirect_map[norm_title] = target
            continue

        if _is_kept_article(page, min_article_chars):
            if len(norm_title.encode("utf-8")) > max_title_bytes:
                skipped_long_titles += 1
                continue
            kept_titles.add(norm_title)

    if skipped_long_titles:
        print(f"wikiconvert: skipped {skipped_long_titles} article(s) with titles longer than "
              f"--max-title-bytes={max_title_bytes} (rerun with a larger value to include them)",
              flush=True)

    sorted_titles = sorted(kept_titles)
    title_to_record_id = {title: i for i, title in enumerate(sorted_titles)}
    return sorted_titles, title_to_record_id, redirect_map


def convert(dump_path, output_dir, chunk_size=65536, max_title_bytes=96,
            min_article_chars=200, max_size_mb=1000, limit=None):
    """ Runs the full two-pass conversion and writes simplewiki.{idx,dat,meta.json}
    into output_dir. Returns the dict written to meta.json. """
    os.makedirs(output_dir, exist_ok=True)

    print("wikiconvert: pass 1/2 -- collecting titles and redirects...")
    sorted_titles, title_to_record_id, redirect_map = _collect_titles_and_redirects(
        dump_path, min_article_chars, max_title_bytes
    )
    if limit is not None:
        sorted_titles = sorted_titles[:limit]
        title_to_record_id = {t: i for i, t in enumerate(sorted_titles)}
    print(f"wikiconvert: keeping {len(sorted_titles)} articles, {len(redirect_map)} redirects")

    # (chunk_id, article_offset, article_len) per record id, backfilled below.
    placements_by_record = [None] * len(sorted_titles)

    def _packed_records():
        print("wikiconvert: pass 2/2 -- cleaning and compressing articles...", flush=True)
        emitted = 0
        start = time.monotonic()
        last_report = start
        for page in iterate_dump_pages(dump_path):
            norm_title = normalize_title(page.title)
            record_id = title_to_record_id.get(norm_title)
            if record_id is None:
                continue  # not in the kept set (redirect/disambig/stub/dropped by --limit)
            if page.redirect_title is not None or parse_redirect_target(page.text) is not None:
                continue

            # Printed *before* cleaning, not after: if clean_wikitext() ever
            # hangs or takes pathologically long on one article (a real risk
            # with malformed/adversarial wikitext, since mwparserfromhell's
            # parse time isn't strictly bounded by article length), this is
            # the one piece of output that tells you which title it's stuck
            # on -- a count-only "N/M articles" heartbeat can't.
            print(f"\r\x1b[Kwikiconvert: [{emitted}/{len(sorted_titles)}] {norm_title}",
                  end="", flush=True)

            body, link_ids = clean_wikitext(page.text, title_to_record_id, redirect_map)
            yield record_id, pack_article_record(link_ids, body)

            emitted += 1
            now = time.monotonic()
            if now - last_report > 5:
                rate = emitted / (now - start) if now > start else 0
                print(f"\r\x1b[Kwikiconvert: ...{emitted}/{len(sorted_titles)} articles "
                      f"({rate:.0f}/s)", flush=True)
                last_report = now

    dat_path = os.path.join(output_dir, "simplewiki.dat")
    chunk_table = []
    with open(dat_path, "wb") as dat_f:
        for chunk_id, (chunk_bytes, placements) in enumerate(_packed_records_chunked(
            _packed_records(), chunk_size
        )):
            compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
            compressed = compressor.compress(chunk_bytes) + compressor.flush()

            data_offset = dat_f.tell()
            dat_f.write(compressed)
            chunk_table.append((data_offset, len(compressed), len(chunk_bytes)))

            for record_id, offset, length in placements:
                placements_by_record[record_id] = (chunk_id, offset, length)

    missing = [i for i, p in enumerate(placements_by_record) if p is None]
    if missing:
        raise RuntimeError(
            f"{len(missing)} kept titles were never emitted in pass 2 -- "
            "pass 1/pass 2 keep-decisions disagree, this is a bug"
        )

    title_record_size = max_title_bytes + 12
    header_size = 32
    chunk_table_offset = header_size
    title_index_offset = chunk_table_offset + len(chunk_table) * 12

    idx_path = os.path.join(output_dir, "simplewiki.idx")
    with open(idx_path, "wb") as idx_f:
        idx_f.write(build_index_header(
            article_count=len(sorted_titles),
            chunk_count=len(chunk_table),
            title_record_size=title_record_size,
            max_title_bytes=max_title_bytes,
            chunk_table_offset=chunk_table_offset,
            title_index_offset=title_index_offset,
        ))
        for entry in chunk_table:
            idx_f.write(pack_chunk_entry(*entry))
        for title in sorted_titles:
            record_id = title_to_record_id[title]
            chunk_id, offset, length = placements_by_record[record_id]
            idx_f.write(pack_title_record(title, chunk_id, offset, length, max_title_bytes))

    total_bytes = os.path.getsize(idx_path) + os.path.getsize(dat_path)
    max_bytes = max_size_mb * 1024 * 1024
    if total_bytes > max_bytes:
        raise RuntimeError(
            f"output is {total_bytes / 1e6:.0f}MB, over --max-size-mb={max_size_mb}. "
            "Simple English Wikipedia is expected to comfortably fit this budget -- "
            "if you're seeing this, something upstream likely changed (dump format, "
            "filters letting too much through); this is a hard fail rather than a "
            "silent truncation because truncating would drop articles arbitrarily."
        )

    meta = {
        "source_url": None,  # filled in by main() when known
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "article_count": len(sorted_titles),
        "chunk_count": len(chunk_table),
        "chunk_size": chunk_size,
        "max_title_bytes": max_title_bytes,
        "format_version": FORMAT_VERSION,
        "total_bytes": total_bytes,
    }
    with open(os.path.join(output_dir, "simplewiki.meta.json"), "w") as meta_f:
        json.dump(meta, meta_f, indent=2)

    print(f"wikiconvert: wrote {len(sorted_titles)} articles, "
          f"{total_bytes / 1e6:.1f}MB total, to {output_dir}")
    return meta


def _packed_records_chunked(records, chunk_size):
    """ Thin adapter: chunk_articles() is generic over any (key, bytes)
    iterable, this just names the specific use for readability at the
    call site above. """
    return chunk_articles(records, chunk_size)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None):
    # stdout is fully (not line-) buffered whenever it isn't a real TTY --
    # notably under `docker run` without -t -- so without this, every
    # print() below would queue up silently instead of showing progress
    # as pass 1/2, pass 2/2, etc. actually happen. Covers every plain
    # print() call; the download progress bar's \r-only updates (no
    # newline) still need their own explicit flush=True regardless,
    # since line buffering only flushes on '\n'.
    sys.stdout.reconfigure(line_buffering=True)

    parser = argparse.ArgumentParser(
        description="Convert a Simple English Wikipedia XML dump into vtOS's "
                     "offline wiki reader format (/sd/wiki/simplewiki.{idx,dat}).",
    )
    parser.add_argument("--url", default=DEFAULT_DUMP_URL,
                         help="Dump URL to download (default: latest simplewiki dump).")
    parser.add_argument("--input", default=None,
                         help="Use a local .xml.bz2 dump instead of downloading.")
    parser.add_argument("--output-dir", default="wiki-data",
                         help="Directory to write simplewiki.{idx,dat,meta.json} into.")
    parser.add_argument("--max-size-mb", type=int, default=1000,
                         help="Hard-fail if output exceeds this size (default: 1000).")
    parser.add_argument("--chunk-size", type=int, default=65536,
                         help="Target decompressed bytes per compressed chunk (default: 65536).")
    parser.add_argument("--max-title-bytes", type=int, default=96,
                         help="Fixed-width title field size in bytes (default: 96).")
    parser.add_argument("--min-article-chars", type=int, default=200,
                         help="Drop pages whose raw wikitext is shorter than this (default: 200).")
    parser.add_argument("--limit", type=int, default=None,
                         help="Only keep the first N articles (sorted by title) -- for quick test runs.")
    parser.add_argument("--force-download", action="store_true",
                         help="Redownload the dump even if a cached copy exists.")
    args = parser.parse_args(argv)

    if args.input:
        dump_path = args.input
    else:
        os.makedirs(args.output_dir, exist_ok=True)
        cache_name = args.url.rsplit("/", 1)[-1]
        dump_path = download_dump(
            args.url, os.path.join(args.output_dir, cache_name), force=args.force_download
        )

    meta_extra_url = args.url if not args.input else None

    convert(
        dump_path, args.output_dir,
        chunk_size=args.chunk_size,
        max_title_bytes=args.max_title_bytes,
        min_article_chars=args.min_article_chars,
        max_size_mb=args.max_size_mb,
        limit=args.limit,
    )

    if meta_extra_url:
        meta_path = os.path.join(args.output_dir, "simplewiki.meta.json")
        with open(meta_path) as f:
            meta = json.load(f)
        meta["source_url"] = meta_extra_url
        with open(meta_path, "w") as f:
            json.dump(meta, f, indent=2)


if __name__ == "__main__":
    main()
