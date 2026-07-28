# Exercises modules/scripts/bin/wiki.py's on-device logic (header
# parsing, binary search, chunk decompression, link-marker rendering)
# under plain CPython. wiki.py can't actually run on MicroPython from
# here, but its pure/structural logic has no MicroPython-only
# dependency besides the `deflate` module, which we fake with a small
# zlib-backed shim -- real bugs in the bisect/offset math or the
# desktop/device format contract show up here well before a firmware
# flash would be needed to find them.

import os
import sys
import types
import zlib

import pytest

import wikiconvert as wc
from tests.test_wikiconvert_integration import _DUMP_XML

_WIKI_BIN_DIR = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "modules", "scripts", "bin")
)


@pytest.fixture(scope="module")
def wiki_module():
    class _FakeDeflateIO:
        def __init__(self, stream, fmt=None, wbits=0, close=False):
            self._stream = stream

        def read(self):
            return zlib.decompressobj(-15).decompress(self._stream.read())

    fake_deflate = types.ModuleType("deflate")
    fake_deflate.DeflateIO = _FakeDeflateIO
    fake_deflate.RAW = "raw"
    sys.modules["deflate"] = fake_deflate
    sys.path.insert(0, _WIKI_BIN_DIR)

    import wiki as wiki_mod

    yield wiki_mod

    sys.path.remove(_WIKI_BIN_DIR)
    del sys.modules["wiki"]
    del sys.modules["deflate"]


@pytest.fixture()
def generated_wiki(tmp_path):
    dump_path = tmp_path / "fixture.xml.bz2"
    import bz2
    dump_path.write_bytes(bz2.compress(_DUMP_XML.encode("utf-8")))

    output_dir = tmp_path / "out"
    wc.convert(str(dump_path), str(output_dir), max_title_bytes=64, min_article_chars=20)
    return str(output_dir / "simplewiki.idx"), str(output_dir / "simplewiki.dat")


def test_normalize_title_matches_desktop_implementation(wiki_module):
    samples = [
        "united states", "United_States_of_America",
        "Python (programming language)#History",
        "  united   states  ", "",
    ]
    for s in samples:
        assert wiki_module.normalize_title(s) == wc.normalize_title(s)


def test_wiki_index_find_exact_and_load_article(wiki_module, generated_wiki):
    idx_path, dat_path = generated_wiki
    wiki = wiki_module.WikiIndex.open(idx_path, dat_path)
    try:
        record_id = wiki.find_exact("Ant")
        assert record_id is not None

        title, body = wiki.load_article(record_id)
        assert title == "Ant"
        text = body.decode("utf-8")
        assert "Infobox" not in text
        assert "A citation" not in text
    finally:
        wiki.close()


def test_wiki_index_find_exact_returns_none_for_missing_title(wiki_module, generated_wiki):
    idx_path, dat_path = generated_wiki
    wiki = wiki_module.WikiIndex.open(idx_path, dat_path)
    try:
        assert wiki.find_exact("Does Not Exist At All") is None
    finally:
        wiki.close()


def test_wiki_index_find_prefix_returns_matches(wiki_module, generated_wiki):
    idx_path, dat_path = generated_wiki
    wiki = wiki_module.WikiIndex.open(idx_path, dat_path)
    try:
        matches = wiki.find_prefix("Un")
        titles = [t for _, t in matches]
        assert "United States" in titles
    finally:
        wiki.close()


def test_render_article_and_link_navigation_round_trip(wiki_module, generated_wiki):
    idx_path, dat_path = generated_wiki
    wiki = wiki_module.WikiIndex.open(idx_path, dat_path)
    try:
        ant_id = wiki.find_exact("Ant")
        title, body = wiki.load_article(ant_id)
        segments = wiki_module._render_article(body)

        # _render_article() only splits on link markers -- it hands plain
        # (text, value) segments to tui.make_pager(), which owns wrapping
        # and link coloring itself now, so no ANSI codes belong here.
        assert not any("\x1b" in text for text, _ in segments)

        link_targets = [value for _, value in segments if value is not None]
        assert len(link_targets) == 2  # Insect, USA->United States redirect

        # Following each link target by record id lands on the expected
        # article -- this is exactly what the PAGE state's Enter handler
        # does (via pager.current_link), without a re-search.
        linked_titles = set()
        for target_id in link_targets:
            linked_title, _ = wiki.load_article(target_id)
            linked_titles.add(linked_title)
        assert linked_titles == {"Insect", "United States"}

        # Plain segments read back as ordinary text with no value.
        plain_text = [text for text, value in segments if value is None]
        assert any("Ants are" in text for text in plain_text)
    finally:
        wiki.close()
