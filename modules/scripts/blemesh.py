#
# MicroPython BLE Flood-Mesh Transport
# Copyright (c) 2026 8bitmcu
# License: MIT
#
# Lightweight mesh networking over BLE legacy advertising broadcasts.
# Not Bluetooth SIG Mesh-compliant -- every node scans continuously and
# relays unseen messages (with TTL + dedup) instead of using GATT
# connections, since connections are point-to-point and don't suit a
# flood mesh.
#
# This module is split into two halves:
#   - Pure protocol logic (below): no `bluetooth`/`machine` imports, so
#     it can be imported and tested under plain CPython
#     (see utils/tests/test_blemesh_device.py).
#   - BLEMesh (device-only): everything that actually touches the radio.
#     `import bluetooth` is deferred into __init__ to keep the module
#     importable on the host for the tests above.

import struct

MAGIC = 0xA5
AD_TYPE_MANUFACTURER_DATA = 0xFF
COMPANY_ID = 0xFFFF  # SIG-reserved for internal/test use, not a real vendor

_HDR_FMT = "<BHHBB"  # magic, msg_id, sender_id, ttl, frag_info
HDR_LEN = struct.calcsize(_HDR_FMT)  # 7

MAX_ADV_LEN = 31
_AD_OVERHEAD = 4  # 1 length byte + 1 type byte + 2 company-id bytes
MAX_PAYLOAD_PER_FRAG = MAX_ADV_LEN - _AD_OVERHEAD - HDR_LEN  # 20
MAX_FRAGS = 16

DEFAULT_TTL = 3
SEEN_CACHE_SIZE = 64
REASSEMBLY_TIMEOUT_MS = 8000
MIN_ADV_HOLD_MS = 150
RELAY_JITTER_MS = (20, 150)
ADV_INTERVAL_US = 100_000
SCAN_INTERVAL_US = 100_000
# Window < interval rather than a continuous 100%-duty scan (window ==
# interval), so WiFi still gets airtime gaps to arbitrate into instead of
# BLE scanning monopolizing the shared 2.4GHz radio. This was dropped as
# far as 30% duty cycle (30_000) while chasing what turned out to be a
# heap-exhaustion hang, not fundamentally a duty-cycle problem (see
# mpconfigboard.h / sdkconfig.board history) -- that made discovery
# noticeably slow (20-30s) without addressing the actual hang. 60% is a
# middle ground: still leaves WiFi real gaps, without being as
# conservative as the value chosen while the wrong cause was suspected.
SCAN_WINDOW_US = 60_000

MAX_MESSAGE_LEN = MAX_PAYLOAD_PER_FRAG * MAX_FRAGS


# ---------------------------------------------------------------------------
# Envelope pack/unpack
# ---------------------------------------------------------------------------

def _pack_frag_info(frag_index, frag_total):
    return ((frag_index & 0x0F) << 4) | ((frag_total - 1) & 0x0F)


def _unpack_frag_info(frag_info):
    return (frag_info >> 4) & 0x0F, (frag_info & 0x0F) + 1


def pack_envelope(msg_id, sender_id, ttl, frag_index, frag_total, payload):
    """Packs a 7-byte header + payload into the mesh wire envelope."""
    header = struct.pack(_HDR_FMT, MAGIC, msg_id, sender_id, ttl,
                          _pack_frag_info(frag_index, frag_total))
    return header + bytes(payload)


def unpack_envelope(raw):
    """Parses a mesh wire envelope. Returns None if malformed or not ours."""
    if len(raw) < HDR_LEN:
        return None

    magic, msg_id, sender_id, ttl, frag_info = struct.unpack(_HDR_FMT, raw[:HDR_LEN])
    if magic != MAGIC:
        return None

    frag_index, frag_total = _unpack_frag_info(frag_info)
    return {
        "msg_id": msg_id,
        "sender_id": sender_id,
        "ttl": ttl,
        "frag_index": frag_index,
        "frag_total": frag_total,
        "payload": raw[HDR_LEN:],
    }


# ---------------------------------------------------------------------------
# Advertising Data (AD) framing
# ---------------------------------------------------------------------------

def build_adv_payload(msg_id, sender_id, ttl, frag_index, frag_total, payload):
    """Wraps an envelope in a Manufacturer Specific Data AD structure."""
    if len(payload) > MAX_PAYLOAD_PER_FRAG:
        raise ValueError(
            f"fragment payload of {len(payload)} bytes exceeds "
            f"MAX_PAYLOAD_PER_FRAG ({MAX_PAYLOAD_PER_FRAG})")

    envelope = pack_envelope(msg_id, sender_id, ttl, frag_index, frag_total, payload)
    body = struct.pack("<H", COMPANY_ID) + envelope
    return bytes([len(body) + 1, AD_TYPE_MANUFACTURER_DATA]) + body


def parse_adv_payload(raw):
    """Extracts a mesh envelope from a raw AD structure, or None if it's
    not a vtOS mesh Manufacturer Specific Data structure."""
    if len(raw) < 4:
        return None

    ad_len, ad_type = raw[0], raw[1]
    if ad_type != AD_TYPE_MANUFACTURER_DATA:
        return None
    if ad_len + 1 > len(raw):
        return None

    company_id = struct.unpack("<H", raw[2:4])[0]
    if company_id != COMPANY_ID:
        return None

    return unpack_envelope(raw[4:ad_len + 1])


# ---------------------------------------------------------------------------
# SeenCache -- bounded dedup cache
# ---------------------------------------------------------------------------

class SeenCache:
    """Bounded FIFO cache of recently-seen (sender_id, msg_id) keys.

    add() is the only entry point: it marks the key as seen *and* reports
    whether it was new, in one call, so callers can't accidentally relay
    or deliver a message before marking it seen (which is what actually
    prevents flood loops).
    """

    def __init__(self, maxsize=SEEN_CACHE_SIZE):
        self._maxsize = maxsize
        self._order = []
        self._set = set()

    def add(self, key):
        if key in self._set:
            return False

        self._set.add(key)
        self._order.append(key)
        if len(self._order) > self._maxsize:
            oldest = self._order.pop(0)
            self._set.discard(oldest)
        return True


# ---------------------------------------------------------------------------
# Reassembler -- joins fragments back into a full message
# ---------------------------------------------------------------------------

class Reassembler:
    """Reassembles fragments for a given (sender_id, msg_id) key.

    Clock values are always passed in by the caller (never read
    internally), so this stays testable under plain CPython with a fake
    clock and works identically once driven by time.ticks_ms() on-device.
    """

    def __init__(self, timeout_ms=REASSEMBLY_TIMEOUT_MS):
        self._timeout_ms = timeout_ms
        self._pending = {}  # key -> {"total": int, "parts": {index: bytes}, "first_seen_ms": int}

    def add_fragment(self, key, frag_index, frag_total, payload, now_ms):
        entry = self._pending.get(key)
        if entry is None:
            entry = {"total": frag_total, "parts": {}, "first_seen_ms": now_ms}
            self._pending[key] = entry

        entry["parts"][frag_index] = payload

        if len(entry["parts"]) < entry["total"]:
            return None

        del self._pending[key]
        return b"".join(entry["parts"][i] for i in range(entry["total"]))

    def sweep(self, now_ms):
        stale = [key for key, entry in self._pending.items()
                 if now_ms - entry["first_seen_ms"] > self._timeout_ms]
        for key in stale:
            del self._pending[key]


def fold_unique_id(raw):
    """XOR-folds an arbitrary-length id (e.g. machine.unique_id(), 6 bytes
    on ESP32) down to the mesh envelope's 16-bit sender_id field. Folding
    rather than truncating means every byte of the source id -- not just
    the first two -- contributes to the result."""
    folded = 0
    for i in range(0, len(raw), 2):
        chunk = raw[i:i + 2]
        value = chunk[0] | (chunk[1] << 8) if len(chunk) == 2 else chunk[0]
        folded ^= value
    return folded


# ---------------------------------------------------------------------------
# BLEMesh -- device-only transport (touches bluetooth/machine/time)
# ---------------------------------------------------------------------------

def _node_id_from_hardware():
    # machine.unique_id() is derived from the SoC's eFuse-burned identity,
    # so it's stable across reboots and reflashes with no need to persist
    # anything to /flash.
    import machine
    return fold_unique_id(machine.unique_id())


class BLEMesh:
    """Flood-mesh transport over BLE legacy advertising.

    Mirrors the poll-based shape of the LoRa `env.radio` transport
    (`receive()` is meant to be called once per loop iteration) so apps
    like blechat.py don't need to know about BLE's IRQ-driven event
    model. There is no start_receive() equivalent: BLE scanning runs
    continuously once this object exists, unlike LoRa's half-duplex
    antenna which must be explicitly re-armed after every TX/RX.
    """

    def __init__(self, node_id=None, ttl_default=DEFAULT_TTL):
        import bluetooth
        import random
        import time

        self._time = time
        self._random = random
        self.ttl_default = ttl_default
        self.node_id = node_id if node_id is not None else _node_id_from_hardware()

        self._seen = SeenCache(SEEN_CACHE_SIZE)
        self._reasm = Reassembler(REASSEMBLY_TIMEOUT_MS)
        self._raw_inbox = []
        self._tx_queue = []  # list of (ready_at_ms, adv_bytes)
        self._delivered = []
        self._last_rssi = None
        self._next_msg_id = 0
        self._adv_active_until_ms = 0

        self._ble = bluetooth.BLE()
        self._ble.active(True)
        self._ble.irq(self._irq)
        self._ble.gap_scan(0, SCAN_INTERVAL_US, SCAN_WINDOW_US, False)

    # -- IRQ handler: kept minimal per MICROPY_PY_BLUETOOTH_USE_SYNC_EVENTS_WITH_INTERLOCK --

    def _irq(self, event, data):
        # This callback runs synchronously against the NimBLE host task
        # (MICROPY_PY_BLUETOOTH_USE_SYNC_EVENTS_WITH_INTERLOCK) -- it must
        # never raise, since an exception escaping here can wedge the
        # interlock and hang the whole device rather than just this app.
        # Every branch below is defensive for exactly that reason.
        try:
            # event 5 == _IRQ_SCAN_RESULT in MicroPython's bluetooth module.
            if event != 5:
                return

            _addr_type, _addr, _adv_type, rssi, adv_data = data
            if len(adv_data) < 2 or adv_data[1] != AD_TYPE_MANUFACTURER_DATA:
                return
            # Cheap magic-byte filter before paying for a full copy.
            if len(adv_data) < 4 + HDR_LEN or adv_data[4] != MAGIC:
                return

            self._raw_inbox.append((bytes(adv_data), rssi))
            if len(self._raw_inbox) > 16:
                self._raw_inbox.pop(0)
        except Exception:
            # Malformed/unexpected event data from the radio -- drop it
            # rather than ever propagating out of IRQ context.
            pass

    # -- public API --

    def broadcast(self, payload, ttl=None):
        payload = bytes(payload)
        if len(payload) > MAX_MESSAGE_LEN:
            raise ValueError(
                f"message of {len(payload)} bytes exceeds MAX_MESSAGE_LEN ({MAX_MESSAGE_LEN})")

        ttl = self.ttl_default if ttl is None else ttl
        msg_id = self._random.getrandbits(16)
        self._seen.add((self.node_id, msg_id))
        self._enqueue_fragments(msg_id, self.node_id, ttl, payload)

    def receive(self):
        # receive() is polled unconditionally every loop iteration by
        # callers (mirroring env.radio.receive()'s contract), so it must
        # never raise -- a transient radio error (e.g. gap_advertise()
        # rejecting a relay because the previous advertisement hasn't
        # settled yet) would otherwise propagate out of the caller's main
        # loop and skip its cleanup (e.g. blechat.py's altscreen exit).
        try:
            self._pump()
        except Exception as e:
            print(f"BLE mesh receive error: {e}")
        return self._delivered.pop(0) if self._delivered else None

    def last_rssi(self):
        return self._last_rssi

    def deinit(self):
        """Releases the radio and its NimBLE host/controller buffers back
        to the heap. Nothing on this instance is usable afterwards --
        callers should also clear their own env.ble = None so the next
        blechat launch builds a fresh BLEMesh() via hardware.init_ble().
        Without this, the BLE stack's memory stays reserved for the rest
        of the boot session even after you're done chatting, which is
        enough on top of another heavy lazy resource (e.g. env.audio) to
        exhaust this chip's scarce internal RAM."""
        try:
            self._ble.gap_scan(None)
        except Exception:
            pass
        try:
            self._ble.active(False)
        except Exception:
            pass

    # -- internal --

    def _enqueue_fragments(self, msg_id, sender_id, ttl, payload):
        frag_total = max(1, -(-len(payload) // MAX_PAYLOAD_PER_FRAG))
        for frag_index in range(frag_total):
            start = frag_index * MAX_PAYLOAD_PER_FRAG
            chunk = payload[start:start + MAX_PAYLOAD_PER_FRAG]
            adv = build_adv_payload(msg_id, sender_id, ttl, frag_index, frag_total, chunk)
            self._tx_queue.append((self._time.ticks_ms(), adv))

    def _pump(self):
        now = self._time.ticks_ms()

        while self._raw_inbox:
            raw, rssi = self._raw_inbox.pop(0)
            parsed = parse_adv_payload(raw)
            if parsed is None:
                continue

            key = (parsed["sender_id"], parsed["msg_id"])
            if not self._seen.add(key):
                continue

            complete = self._reasm.add_fragment(
                key, parsed["frag_index"], parsed["frag_total"], parsed["payload"], now)
            if complete is not None:
                self._delivered.append(complete)
                self._last_rssi = rssi

            if parsed["ttl"] > 0:
                jitter = self._random.randint(*RELAY_JITTER_MS)
                adv = build_adv_payload(
                    parsed["msg_id"], parsed["sender_id"], parsed["ttl"] - 1,
                    parsed["frag_index"], parsed["frag_total"], parsed["payload"])
                self._tx_queue.append((self._time.ticks_add(now, jitter), adv))

        self._reasm.sweep(now)
        self._drive_tx(now)

    def _drive_tx(self, now):
        if self._time.ticks_diff(now, self._adv_active_until_ms) < 0:
            return
        if not self._tx_queue:
            return

        ready_at, adv = self._tx_queue[0]
        if self._time.ticks_diff(now, ready_at) < 0:
            return

        self._tx_queue.pop(0)
        # Stop scanning before switching the radio into advertiser mode,
        # then resume right after, instead of running both roles fully
        # concurrently. This board has CONFIG_ESP_TASK_WDT=n (see
        # sdkconfig.board), so if a scan-result IRQ delivery and this
        # synchronous gap_advertise() call ever raced against each other
        # inside the NimBLE host task, there would be nothing to recover
        # from a resulting deadlock -- serializing the two roles here
        # narrows that window.
        self._ble.gap_scan(None)
        self._ble.gap_advertise(ADV_INTERVAL_US, adv_data=adv, connectable=False)
        self._ble.gap_scan(0, SCAN_INTERVAL_US, SCAN_WINDOW_US, False)
        self._adv_active_until_ms = self._time.ticks_add(now, MIN_ADV_HOLD_MS)
