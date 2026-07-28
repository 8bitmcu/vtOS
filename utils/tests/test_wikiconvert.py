import struct

import pytest

import wikiconvert as wc


# ---------------------------------------------------------------------------
# normalize_title
# ---------------------------------------------------------------------------

def test_normalize_title_capitalizes_first_letter():
    assert wc.normalize_title("united states") == "United states"


def test_normalize_title_leaves_already_capitalized_alone():
    assert wc.normalize_title("United States") == "United States"


def test_normalize_title_converts_underscores_to_spaces():
    assert wc.normalize_title("United_States_of_America") == "United States of America"


def test_normalize_title_strips_section_anchor():
    assert wc.normalize_title("Python (programming language)#History") == "Python (programming language)"


def test_normalize_title_collapses_whitespace_and_strips_ends():
    assert wc.normalize_title("  united   states  ") == "United states"


def test_normalize_title_empty_string_stays_empty():
    assert wc.normalize_title("") == ""


# ---------------------------------------------------------------------------
# index header pack/unpack
# ---------------------------------------------------------------------------

def test_build_and_parse_index_header_round_trips():
    header = wc.build_index_header(
        article_count=1234,
        chunk_count=56,
        title_record_size=108,
        max_title_bytes=96,
        chunk_table_offset=32,
        title_index_offset=32 + 56 * 12,
    )
    assert len(header) == 32

    parsed = wc.parse_index_header(header)
    assert parsed.magic == wc.MAGIC
    assert parsed.version == wc.FORMAT_VERSION
    assert parsed.article_count == 1234
    assert parsed.chunk_count == 56
    assert parsed.title_record_size == 108
    assert parsed.max_title_bytes == 96
    assert parsed.chunk_table_offset == 32
    assert parsed.title_index_offset == 32 + 56 * 12


def test_parse_index_header_rejects_bad_magic():
    bogus = struct.pack("<4sHHIIHHII4x", b"NOPE", 1, 0, 0, 0, 0, 0, 0, 0)
    with pytest.raises(ValueError):
        wc.parse_index_header(bogus)


def test_parse_index_header_rejects_unsupported_version():
    header = wc.build_index_header(
        article_count=0, chunk_count=0,
        title_record_size=0, max_title_bytes=0,
        chunk_table_offset=32, title_index_offset=32,
    )
    tampered = header[:4] + struct.pack("<H", 99) + header[6:]
    with pytest.raises(ValueError):
        wc.parse_index_header(tampered)


# ---------------------------------------------------------------------------
# chunk table entry pack/unpack
# ---------------------------------------------------------------------------

def test_pack_and_unpack_chunk_entry_round_trips():
    packed = wc.pack_chunk_entry(data_offset=4096, compressed_len=1000, decompressed_len=6553)
    assert len(packed) == 12
    assert wc.unpack_chunk_entry(packed) == (4096, 1000, 6553)


# ---------------------------------------------------------------------------
# title record pack/unpack
# ---------------------------------------------------------------------------

def test_pack_and_unpack_title_record_round_trips():
    packed = wc.pack_title_record("United States", chunk_id=3, article_offset=128, article_len=4096, max_title_bytes=32)
    assert len(packed) == 32 + 12

    title, chunk_id, article_offset, article_len = wc.unpack_title_record(packed, max_title_bytes=32)
    assert title == "United States"
    assert chunk_id == 3
    assert article_offset == 128
    assert article_len == 4096


def test_pack_title_record_pads_short_titles_with_nul():
    packed = wc.pack_title_record("Ant", chunk_id=0, article_offset=0, article_len=0, max_title_bytes=8)
    assert packed[:8] == b"Ant\x00\x00\x00\x00\x00"


def test_pack_title_record_rejects_title_too_long_for_field():
    with pytest.raises(ValueError):
        wc.pack_title_record("A" * 200, chunk_id=0, article_offset=0, article_len=0, max_title_bytes=96)


def test_title_records_sort_correctly_as_raw_bytes():
    # NUL-padding must sort before any printable character so a plain
    # byte-wise binary search over fixed-width records works on-device
    # without decoding each candidate first.
    packed_short = wc.pack_title_record("Ant", 0, 0, 0, max_title_bytes=8)
    packed_long = wc.pack_title_record("Ants", 0, 0, 0, max_title_bytes=8)
    assert packed_short < packed_long


# ---------------------------------------------------------------------------
# article record pack/unpack
# ---------------------------------------------------------------------------

def test_pack_and_unpack_article_record_round_trips_with_links():
    body = "Hello \x1e42\x1fworld\x1e!".encode("utf-8")
    packed = wc.pack_article_record(link_ids=[42, 7], body=body)

    link_ids, unpacked_body = wc.unpack_article_record(packed)
    assert link_ids == [42, 7]
    assert unpacked_body == body


def test_pack_and_unpack_article_record_with_no_links():
    body = "just plain text".encode("utf-8")
    packed = wc.pack_article_record(link_ids=[], body=body)

    link_ids, unpacked_body = wc.unpack_article_record(packed)
    assert link_ids == []
    assert unpacked_body == body
