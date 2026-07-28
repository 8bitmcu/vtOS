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

import re
import struct

import mwparserfromhell

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
