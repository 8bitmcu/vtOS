# Exercises modules/scripts/lib/blemesh.py's pure protocol logic (envelope
# pack/unpack, advertising-payload framing, dedup cache, fragment
# reassembly) under plain CPython. The BLEMesh transport class itself
# needs a real bluetooth.BLE()/NimBLE radio and can't run here, but the
# `import bluetooth` it depends on is deferred into __init__ specifically
# so this module-level logic has no MicroPython-only dependency at all.

import os
import sys

import pytest

_BLEMESH_DIR = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "modules", "scripts", "lib")
)


@pytest.fixture(scope="module")
def bm():
    sys.path.insert(0, _BLEMESH_DIR)
    import blemesh as bm_mod

    yield bm_mod

    sys.path.remove(_BLEMESH_DIR)
    del sys.modules["blemesh"]


# ---------------------------------------------------------------------------
# pack_envelope / unpack_envelope
# ---------------------------------------------------------------------------

def test_pack_and_unpack_envelope_round_trips(bm):
    raw = bm.pack_envelope(msg_id=1234, sender_id=5678, ttl=3,
                            frag_index=0, frag_total=1, payload=b"hello")
    parsed = bm.unpack_envelope(raw)
    assert parsed["msg_id"] == 1234
    assert parsed["sender_id"] == 5678
    assert parsed["ttl"] == 3
    assert parsed["frag_index"] == 0
    assert parsed["frag_total"] == 1
    assert parsed["payload"] == b"hello"


def test_unpack_envelope_rejects_bad_magic(bm):
    raw = bm.pack_envelope(msg_id=1, sender_id=1, ttl=1,
                            frag_index=0, frag_total=1, payload=b"x")
    tampered = bytes([0x00]) + raw[1:]
    assert bm.unpack_envelope(tampered) is None


def test_unpack_envelope_rejects_short_packet(bm):
    assert bm.unpack_envelope(b"\xa5\x01") is None


def test_pack_envelope_frag_info_round_trips_max_values(bm):
    raw = bm.pack_envelope(msg_id=0xFFFF, sender_id=0xFFFF, ttl=255,
                            frag_index=15, frag_total=16, payload=b"")
    parsed = bm.unpack_envelope(raw)
    assert parsed["frag_index"] == 15
    assert parsed["frag_total"] == 16


# ---------------------------------------------------------------------------
# build_adv_payload / parse_adv_payload
# ---------------------------------------------------------------------------

def test_build_and_parse_adv_payload_round_trips(bm):
    raw = bm.build_adv_payload(msg_id=42, sender_id=7, ttl=2,
                                frag_index=0, frag_total=1, payload=b"hi")
    parsed = bm.parse_adv_payload(raw)
    assert parsed["msg_id"] == 42
    assert parsed["sender_id"] == 7
    assert parsed["ttl"] == 2
    assert parsed["payload"] == b"hi"


def test_build_adv_payload_fits_legacy_advertising_budget(bm):
    payload = b"x" * bm.MAX_PAYLOAD_PER_FRAG
    raw = bm.build_adv_payload(msg_id=1, sender_id=1, ttl=1,
                                frag_index=0, frag_total=1, payload=payload)
    assert len(raw) == bm.MAX_ADV_LEN


def test_build_adv_payload_rejects_oversized_fragment(bm):
    with pytest.raises(ValueError):
        bm.build_adv_payload(msg_id=1, sender_id=1, ttl=1, frag_index=0,
                              frag_total=1, payload=b"x" * (bm.MAX_PAYLOAD_PER_FRAG + 1))


def test_parse_adv_payload_ignores_non_manufacturer_ad_type(bm):
    # AD length=3, AD type=0x09 (Complete Local Name), 2 bytes of "data"
    raw = bytes([3, 0x09]) + b"ab"
    assert bm.parse_adv_payload(raw) is None


def test_parse_adv_payload_ignores_foreign_company_id(bm):
    import struct
    body = struct.pack("<H", 0x1234) + bm.pack_envelope(
        msg_id=1, sender_id=1, ttl=1, frag_index=0, frag_total=1, payload=b"x")
    raw = bytes([len(body) + 1, 0xFF]) + body
    assert bm.parse_adv_payload(raw) is None


# ---------------------------------------------------------------------------
# SeenCache
# ---------------------------------------------------------------------------

def test_seen_cache_add_returns_true_for_new_key(bm):
    cache = bm.SeenCache(maxsize=4)
    assert cache.add((1, 2)) is True


def test_seen_cache_add_returns_false_for_duplicate_key(bm):
    cache = bm.SeenCache(maxsize=4)
    cache.add((1, 2))
    assert cache.add((1, 2)) is False


def test_seen_cache_evicts_oldest_entry_past_maxsize(bm):
    cache = bm.SeenCache(maxsize=2)
    cache.add((1, 1))
    cache.add((2, 2))
    cache.add((3, 3))  # evicts (1, 1)
    assert cache.add((1, 1)) is True  # treated as new again
    assert cache.add((3, 3)) is False  # still remembered


# ---------------------------------------------------------------------------
# Reassembler
# ---------------------------------------------------------------------------

def test_reassembler_single_fragment_completes_immediately(bm):
    r = bm.Reassembler()
    result = r.add_fragment(key=(1, 1), frag_index=0, frag_total=1, payload=b"hi", now_ms=0)
    assert result == b"hi"


def test_reassembler_waits_for_all_fragments(bm):
    r = bm.Reassembler()
    result = r.add_fragment(key=(1, 1), frag_index=0, frag_total=2, payload=b"he", now_ms=0)
    assert result is None
    result = r.add_fragment(key=(1, 1), frag_index=1, frag_total=2, payload=b"llo", now_ms=1)
    assert result == b"hello"


def test_reassembler_handles_out_of_order_fragments(bm):
    r = bm.Reassembler()
    assert r.add_fragment(key=(1, 1), frag_index=2, frag_total=3, payload=b"C", now_ms=0) is None
    assert r.add_fragment(key=(1, 1), frag_index=0, frag_total=3, payload=b"A", now_ms=0) is None
    result = r.add_fragment(key=(1, 1), frag_index=1, frag_total=3, payload=b"B", now_ms=0)
    assert result == b"ABC"


def test_reassembler_sweep_drops_stale_incomplete_entries(bm):
    r = bm.Reassembler(timeout_ms=1000)
    r.add_fragment(key=(1, 1), frag_index=0, frag_total=2, payload=b"he", now_ms=0)
    r.sweep(now_ms=5000)
    # Entry was dropped by the sweep, so this is treated as a fresh start,
    # not the second half of the original message.
    result = r.add_fragment(key=(1, 1), frag_index=1, frag_total=2, payload=b"llo", now_ms=5001)
    assert result is None


def test_reassembler_keeps_fresh_entries_on_sweep(bm):
    r = bm.Reassembler(timeout_ms=1000)
    r.add_fragment(key=(1, 1), frag_index=0, frag_total=2, payload=b"he", now_ms=0)
    r.sweep(now_ms=500)
    result = r.add_fragment(key=(1, 1), frag_index=1, frag_total=2, payload=b"llo", now_ms=600)
    assert result == b"hello"


# ---------------------------------------------------------------------------
# fold_unique_id
# ---------------------------------------------------------------------------

def test_fold_unique_id_fits_16_bits(bm):
    folded = bm.fold_unique_id(b"\x01\x02\x03\x04\x05\x06")
    assert 0 <= folded <= 0xFFFF


def test_fold_unique_id_is_deterministic(bm):
    raw = b"\xAA\xBB\xCC\xDD\xEE\xFF"
    assert bm.fold_unique_id(raw) == bm.fold_unique_id(raw)


def test_fold_unique_id_xors_two_byte_chunks_little_endian(bm):
    # 0x0201 ^ 0x0403 ^ 0x0605
    assert bm.fold_unique_id(b"\x01\x02\x03\x04\x05\x06") == (0x0201 ^ 0x0403 ^ 0x0605)


def test_fold_unique_id_handles_odd_length_input(bm):
    # Trailing single byte contributes its raw value, not paired with anything.
    assert bm.fold_unique_id(b"\x01\x02\x03") == (0x0201 ^ 0x03)


def test_fold_unique_id_differs_when_trailing_byte_differs(bm):
    a = bm.fold_unique_id(b"\x01\x02\x03\x04\x05\x06")
    b = bm.fold_unique_id(b"\x01\x02\x03\x04\x05\x07")
    assert a != b
