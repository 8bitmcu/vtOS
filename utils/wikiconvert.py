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
