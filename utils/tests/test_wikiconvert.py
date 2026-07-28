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


# ---------------------------------------------------------------------------
# namespace / redirect / disambiguation predicates
# ---------------------------------------------------------------------------

def test_is_article_namespace_accepts_ns_zero_as_string():
    assert wc.is_article_namespace("0") is True


def test_is_article_namespace_rejects_other_namespaces():
    assert wc.is_article_namespace("14") is False   # Category
    assert wc.is_article_namespace("6") is False    # File
    assert wc.is_article_namespace("Talk") is False


def test_parse_redirect_target_extracts_normalized_title():
    assert wc.parse_redirect_target("#REDIRECT [[United States]]") == "United States"


def test_parse_redirect_target_is_case_insensitive_and_allows_leading_space():
    assert wc.parse_redirect_target("  #redirect [[united states]]") == "United states"


def test_parse_redirect_target_strips_piped_display_text():
    assert wc.parse_redirect_target("#REDIRECT [[United States|USA]]") == "United States"


def test_parse_redirect_target_returns_none_for_non_redirect_page():
    assert wc.parse_redirect_target("This is a normal article about ants.") is None


def test_is_disambiguation_detects_disambig_template():
    assert wc.is_disambiguation("'''Mercury''' may refer to:\n{{disambig}}") is True


def test_is_disambiguation_detects_disambiguation_category():
    text = "Foo may refer to several things.\n[[Category:Disambiguation pages]]"
    assert wc.is_disambiguation(text) is True


def test_is_disambiguation_false_for_regular_article():
    assert wc.is_disambiguation("Ants are insects that live in colonies.") is False


# ---------------------------------------------------------------------------
# chunk_articles
# ---------------------------------------------------------------------------

def test_chunk_articles_empty_input_yields_nothing():
    assert list(wc.chunk_articles([], chunk_size=100)) == []


def test_chunk_articles_single_small_record():
    chunks = list(wc.chunk_articles([("a", b"hello")], chunk_size=100))
    assert len(chunks) == 1
    data, placements = chunks[0]
    assert data == b"hello"
    assert placements == [("a", 0, 5)]


def test_chunk_articles_combines_records_under_threshold():
    records = [("a", b"12345"), ("b", b"67890")]
    chunks = list(wc.chunk_articles(records, chunk_size=100))
    assert len(chunks) == 1
    data, placements = chunks[0]
    assert data == b"1234567890"
    assert placements == [("a", 0, 5), ("b", 5, 5)]


def test_chunk_articles_flushes_at_threshold_boundary():
    # chunk_size=10: two 5-byte records exactly fill one chunk without
    # flushing early; a third record starts a fresh chunk.
    records = [("a", b"12345"), ("b", b"67890"), ("c", b"xyz")]
    chunks = list(wc.chunk_articles(records, chunk_size=10))
    assert len(chunks) == 2
    assert chunks[0][0] == b"1234567890"
    assert chunks[0][1] == [("a", 0, 5), ("b", 5, 5)]
    assert chunks[1][0] == b"xyz"
    assert chunks[1][1] == [("c", 0, 3)]


def test_chunk_articles_oversized_record_gets_its_own_chunk():
    big = b"x" * 500
    records = [("small1", b"abc"), ("big", big), ("small2", b"def")]
    chunks = list(wc.chunk_articles(records, chunk_size=100))
    assert len(chunks) == 3
    assert chunks[0] == (b"abc", [("small1", 0, 3)])
    assert chunks[1] == (big, [("big", 0, 500)])
    assert chunks[2] == (b"def", [("small2", 0, 3)])
