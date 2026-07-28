import bz2
import zlib

import wikiconvert as wc

_DUMP_XML = """<mediawiki xmlns="http://www.mediawiki.org/xml/export-0.10/">
  <page>
    <title>Ant</title>
    <ns>0</ns>
    <id>1</id>
    <revision>
      <text xml:space="preserve">{{Infobox animal|name=Ant}}
'''Ants''' are [[Insect|insects]] that live in [[USA|the United States]] and
elsewhere. See [[Nonexistent Topic|here]] for more.&lt;ref&gt;A citation.&lt;/ref&gt;
</text>
    </revision>
  </page>
  <page>
    <title>Insect</title>
    <ns>0</ns>
    <id>2</id>
    <revision>
      <text xml:space="preserve">An '''insect''' is a small animal with six legs.</text>
    </revision>
  </page>
  <page>
    <title>USA</title>
    <ns>0</ns>
    <id>3</id>
    <redirect title="United States" />
    <revision>
      <text xml:space="preserve">#REDIRECT [[United States]]</text>
    </revision>
  </page>
  <page>
    <title>United States</title>
    <ns>0</ns>
    <id>4</id>
    <revision>
      <text xml:space="preserve">The '''United States''' is a country in North America.</text>
    </revision>
  </page>
  <page>
    <title>Mercury (disambiguation)</title>
    <ns>0</ns>
    <id>5</id>
    <revision>
      <text xml:space="preserve">'''Mercury''' may refer to several things.
{{disambig}}
</text>
    </revision>
  </page>
  <page>
    <title>Stub Article</title>
    <ns>0</ns>
    <id>6</id>
    <revision>
      <text xml:space="preserve">Too short.</text>
    </revision>
  </page>
  <page>
    <title>Category:Animals</title>
    <ns>14</ns>
    <id>7</id>
    <revision>
      <text xml:space="preserve">Articles about animals.</text>
    </revision>
  </page>
</mediawiki>
"""


def _write_fixture_dump(tmp_path):
    dump_path = tmp_path / "fixture-pages-articles.xml.bz2"
    dump_path.write_bytes(bz2.compress(_DUMP_XML.encode("utf-8")))
    return str(dump_path)


def _read_chunk_table(raw, header):
    entries = []
    off = header.chunk_table_offset
    for _ in range(header.chunk_count):
        entries.append(wc.unpack_chunk_entry(raw[off:off + 12]))
        off += 12
    return entries


def test_convert_end_to_end_on_synthetic_dump(tmp_path):
    dump_path = _write_fixture_dump(tmp_path)
    output_dir = tmp_path / "out"

    meta = wc.convert(
        dump_path, str(output_dir),
        chunk_size=65536, max_title_bytes=64,
        min_article_chars=20, max_size_mb=1000,
    )

    idx_path = output_dir / "simplewiki.idx"
    dat_path = output_dir / "simplewiki.dat"
    assert idx_path.exists()
    assert dat_path.exists()
    assert meta["article_count"] == 3  # Ant, Insect, United States (redirect/disambig/stub/category dropped)

    raw_idx = idx_path.read_bytes()
    header = wc.parse_index_header(raw_idx)
    assert header.article_count == 3

    titles = {}
    off = header.title_index_offset
    for _ in range(header.article_count):
        rec = raw_idx[off:off + header.title_record_size]
        title, chunk_id, article_offset, article_len = wc.unpack_title_record(
            rec, header.max_title_bytes
        )
        titles[title] = (chunk_id, article_offset, article_len)
        off += header.title_record_size

    assert set(titles) == {"Ant", "Insect", "United States"}
    # Title records must be written in sorted order -- the on-device
    # reader binary searches this file assuming that invariant holds.
    assert list(titles) == sorted(titles)

    chunk_table = _read_chunk_table(raw_idx, header)
    dat_bytes = dat_path.read_bytes()

    def load_article(title):
        chunk_id, article_offset, article_len = titles[title]
        data_offset, compressed_len, decompressed_len = chunk_table[chunk_id]
        compressed = dat_bytes[data_offset:data_offset + compressed_len]
        raw_chunk = zlib.decompressobj(-15).decompress(compressed)
        assert len(raw_chunk) == decompressed_len
        record_bytes = raw_chunk[article_offset:article_offset + article_len]
        return wc.unpack_article_record(record_bytes)

    link_ids, body = load_article("Ant")
    text = body.decode("utf-8")

    # Template and <ref> content must not leak into the cleaned body.
    assert "Infobox" not in text
    assert "A citation" not in text
    # Bold markup collapses to plain text.
    assert "Ants are" in text
    # The dead link ([[Nonexistent Topic|here]]) renders as plain display
    # text with no marker -- only the two resolvable links (Insect,
    # USA->United States via redirect) count as real links.
    assert "here" in text
    assert len(link_ids) == 2

    sorted_titles = sorted(titles)
    insect_record_id = sorted_titles.index("Insect")
    us_record_id = sorted_titles.index("United States")
    assert insect_record_id in link_ids
    assert us_record_id in link_ids  # confirms USA -> United States redirect resolved, not a dead link

    for title in ("Insect", "United States"):
        _, body = load_article(title)
        assert len(body) > 0
