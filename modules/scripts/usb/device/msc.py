#
# USB Mass Storage Class (Bulk-Only Transport) driver
# Copyright (c) 2026 8bitmcu
# License: MIT
#
# No usb-device-msc package exists in micropython-lib (only cdc/hid/midi/
# keyboard/mouse do), so this implements the SCSI Bulk-Only Transport
# protocol directly on top of usb.device.core.Interface, the same generic
# framework those drivers use. Only a single LUN (LUN 0) is supported, and
# only the minimal SCSI command set that Windows/macOS/Linux actually send
# to a plain USB flash drive.
#
import struct
from micropython import const

from .core import Interface

_EP_IN_FLAG = const(1 << 7)

_INTERFACE_CLASS_MSC = const(0x08)
_SUBCLASS_SCSI = const(0x06)
_PROTOCOL_BULK_ONLY = const(0x50)

_REQ_TYPE_CLASS = const(0x1)
_MSC_REQ_GET_MAX_LUN = const(0xFE)
_MSC_REQ_RESET = const(0xFF)

_STAGE_SETUP = const(1)

_BULK_EP_LEN = const(64)

# Number of 512-byte blocks moved per USB bulk transfer during READ10/WRITE10.
# Bounds the scratch buffer and keeps each SPI transfer (and each pause of
# the shared display bus, see hardware.FastSDCard) reasonably short.
_XFER_BLOCKS = const(8)

_CBW_SIG = const(0x43425355)
_CBW_LEN = const(31)
_CSW_SIG = const(0x53425355)
_CSW_LEN = const(13)

_CSW_STATUS_OK = const(0)
_CSW_STATUS_FAIL = const(1)

_SCSI_TEST_UNIT_READY = const(0x00)
_SCSI_REQUEST_SENSE = const(0x03)
_SCSI_INQUIRY = const(0x12)
_SCSI_MODE_SENSE6 = const(0x1A)
_SCSI_START_STOP_UNIT = const(0x1B)
_SCSI_PREVENT_ALLOW_REMOVAL = const(0x1E)
_SCSI_READ_CAPACITY10 = const(0x25)
_SCSI_READ10 = const(0x28)
_SCSI_WRITE10 = const(0x2A)
_SCSI_SYNCHRONIZE_CACHE10 = const(0x35)

_SENSE_NO_SENSE = const(0x00)
_SENSE_MEDIUM_ERROR = const(0x03)
_SENSE_ILLEGAL_REQUEST = const(0x05)

_ASC_INVALID_COMMAND_OPCODE = const(0x20)
_ASC_LBA_OUT_OF_RANGE = const(0x21)
_ASC_UNRECOVERED_READ_ERROR = const(0x11)
_ASC_WRITE_FAULT = const(0x03)

_ST_CBW = const(0)      # waiting for a Command Block Wrapper
_ST_DATA_IN = const(1)  # sending response/read data to the host
_ST_DATA_OUT = const(2) # receiving write data from the host
_ST_STATUS = const(3)   # sending the Command Status Wrapper


def _pad(s, n):
    # MicroPython's str has no ljust()/rjust() -- truncate/space-pad to
    # exactly n characters for the fixed-width INQUIRY vendor/product/
    # revision fields.
    s = s[:n]
    return s + " " * (n - len(s))


class MSCInterface(Interface):
    # USB Mass Storage Class interface (SCSI transparent command set,
    # Bulk-Only Transport). Wraps any object implementing the standard
    # MicroPython block device protocol (readblocks/writeblocks/ioctl with
    # BP_IOCTL_SEC_COUNT=4/BP_IOCTL_SEC_SIZE=5) -- the same protocol
    # os.VfsFat/os.VfsLfs2 use -- and exposes it to the USB host as a plain
    # USB drive.
    def __init__(self, block_device, removable=True, vendor="8bitmcu", product="vtOS Storage", rev="1.0"):
        super().__init__()
        self.bd = block_device
        self._removable = removable
        self._vendor = _pad(vendor, 8)
        self._product = _pad(product, 16)
        self._rev = _pad(rev, 4)

        self.ep_out = None
        self.ep_in = None

        self._state = _ST_CBW
        self._cbw = bytearray(_CBW_LEN)
        self._csw = bytearray(_CSW_LEN)
        self._xfer_buf = bytearray(_XFER_BLOCKS * 512)

        self._sense = (_SENSE_NO_SENSE, 0, 0)  # (key, asc, ascq) for REQUEST SENSE

        self._tag = 0
        self._status = _CSW_STATUS_OK
        self._residue = 0

        self._lba = 0
        self._blocks_left = 0
        self._pending_write_blocks = 0

        self._block_size = 512
        self._block_count = 0

    ###
    ### USB Interface Implementation
    ###

    def desc_cfg(self, desc, itf_num, ep_num, strs):
        desc.interface(itf_num, 2, _INTERFACE_CLASS_MSC, _SUBCLASS_SCSI, _PROTOCOL_BULK_ONLY)

        self.ep_out = ep_num
        self.ep_in = ep_num | _EP_IN_FLAG
        desc.endpoint(self.ep_out, "bulk", _BULK_EP_LEN, 0)
        desc.endpoint(self.ep_in, "bulk", _BULK_EP_LEN, 0)

    def num_itfs(self):
        return 1

    def num_eps(self):
        return 1  # one ep_num, shared by ep_out/ep_in

    def on_open(self):
        super().on_open()
        try:
            self._block_count = self.bd.ioctl(4, 0)
            self._block_size = self.bd.ioctl(5, 0)
        except OSError:
            self._block_count = 0
            self._block_size = 512
        self._state = _ST_CBW
        self._arm_cbw_read()

    def on_reset(self):
        super().on_reset()
        self._state = _ST_CBW

    def on_interface_control_xfer(self, stage, request):
        # Handle the two MSC class-specific control requests: Get Max LUN
        # and Bulk-Only Mass Storage Reset. See USB Mass Storage Class
        # Bulk-Only Transport spec, section 3.
        bmRequestType, bRequest, wValue, wIndex, wLength = struct.unpack("BBHHH", request)
        req_type = (bmRequestType >> 5) & 0x03

        if req_type != _REQ_TYPE_CLASS:
            return False

        if bRequest == _MSC_REQ_GET_MAX_LUN:
            if stage == _STAGE_SETUP:
                return b"\x00"  # one LUN (LUN 0), max index 0
            return True

        if bRequest == _MSC_REQ_RESET:
            if stage == _STAGE_SETUP and wLength != 0:
                return False
            if stage == _STAGE_SETUP:
                self._state = _ST_CBW
            return True

        return False

    ###
    ### Command Block Wrapper / Command Status Wrapper handling
    ###

    def _arm_cbw_read(self):
        if self.is_open() and not self.xfer_pending(self.ep_out):
            self._state = _ST_CBW
            self.submit_xfer(self.ep_out, self._cbw, self._cbw_done)

    def _cbw_done(self, ep, res, n):
        if res != 0 or n != _CBW_LEN:
            self._arm_cbw_read()  # malformed/short CBW -- just wait for the next one
            return

        sig, tag, data_len, flags, lun, cb_len = struct.unpack_from("<IIIBBB", self._cbw, 0)
        if sig != _CBW_SIG or cb_len < 1 or cb_len > 16:
            self._arm_cbw_read()
            return

        self._tag = tag
        self._residue = data_len
        self._status = _CSW_STATUS_OK
        cb = self._cbw[15 : 15 + cb_len]
        self._dispatch(cb, data_len)

    def _set_sense(self, key, asc, ascq=0):
        self._sense = (key, asc, ascq)

    def _fail(self, key, asc, ascq=0):
        self._set_sense(key, asc, ascq)
        self._status = _CSW_STATUS_FAIL
        self._send_csw()

    def _send_csw(self):
        struct.pack_into("<IIIB", self._csw, 0, _CSW_SIG, self._tag, self._residue, self._status)
        self._state = _ST_STATUS
        self.submit_xfer(self.ep_in, self._csw, self._csw_done)

    def _csw_done(self, ep, res, n):
        self._arm_cbw_read()

    ###
    ### SCSI command dispatch
    ###

    def _dispatch(self, cb, data_len):
        op = cb[0]

        if op == _SCSI_TEST_UNIT_READY:
            self._set_sense(_SENSE_NO_SENSE, 0)
            self._send_csw()

        elif op == _SCSI_REQUEST_SENSE:
            self._send_data_in(self._build_sense(), data_len)

        elif op == _SCSI_INQUIRY:
            self._send_data_in(self._build_inquiry(), data_len)

        elif op == _SCSI_MODE_SENSE6:
            self._send_data_in(self._build_mode_sense6(), data_len)

        elif op == _SCSI_READ_CAPACITY10:
            self._send_data_in(self._build_read_capacity10(), data_len)

        elif op in (_SCSI_START_STOP_UNIT, _SCSI_PREVENT_ALLOW_REMOVAL, _SCSI_SYNCHRONIZE_CACHE10):
            self._set_sense(_SENSE_NO_SENSE, 0)
            self._send_csw()

        elif op == _SCSI_READ10:
            self._start_read10(cb)

        elif op == _SCSI_WRITE10:
            self._start_write10(cb)

        else:
            self._fail(_SENSE_ILLEGAL_REQUEST, _ASC_INVALID_COMMAND_OPCODE)

    def _send_data_in(self, data, alloc_len):
        # Fixed-size SCSI response (INQUIRY/MODE SENSE/READ CAPACITY/REQUEST
        # SENSE) -- send min(our data, what the host said it would accept).
        n = min(len(data), alloc_len)
        self._residue = alloc_len - n
        self._state = _ST_DATA_IN
        self.submit_xfer(self.ep_in, data[:n], self._data_in_done)

    def _data_in_done(self, ep, res, n):
        if res != 0:
            self._status = _CSW_STATUS_FAIL
        self._send_csw()

    def _build_sense(self):
        key, asc, ascq = self._sense
        buf = bytearray(18)
        buf[0] = 0x70
        buf[2] = key & 0x0F
        buf[7] = 10
        buf[12] = asc
        buf[13] = ascq
        return buf

    def _build_inquiry(self):
        buf = bytearray(36)
        buf[0] = 0x00  # direct access block device
        buf[1] = 0x80 if self._removable else 0x00
        buf[2] = 0x00  # version
        buf[3] = 0x02  # response data format
        buf[4] = 31    # additional length
        buf[8:16] = self._vendor.encode()
        buf[16:32] = self._product.encode()
        buf[32:36] = self._rev.encode()
        return buf

    def _build_mode_sense6(self):
        # Mode data length, medium type, device-specific param (bit7 = write
        # protect, always 0 here), block descriptor length.
        return bytes([3, 0, 0, 0])

    def _build_read_capacity10(self):
        last_lba = max(self._block_count - 1, 0)
        return struct.pack(">II", last_lba, self._block_size)

    ###
    ### READ10 / WRITE10 -- transferred in chunks of _XFER_BLOCKS blocks
    ###

    def _parse_rw10(self, cb):
        lba = struct.unpack_from(">I", cb, 2)[0]
        blocks = struct.unpack_from(">H", cb, 7)[0]
        return lba, blocks

    def _start_read10(self, cb):
        lba, blocks = self._parse_rw10(cb)
        if lba + blocks > self._block_count:
            self._fail(_SENSE_ILLEGAL_REQUEST, _ASC_LBA_OUT_OF_RANGE)
            return
        self._lba = lba
        self._blocks_left = blocks
        self._state = _ST_DATA_IN
        self._send_next_read_chunk()

    def _send_next_read_chunk(self):
        if self._blocks_left <= 0:
            self._status = _CSW_STATUS_OK
            self._residue = 0
            self._send_csw()
            return

        n = min(self._blocks_left, _XFER_BLOCKS)
        buf = memoryview(self._xfer_buf)[: n * self._block_size]
        try:
            self.bd.readblocks(self._lba, buf)
        except OSError:
            self._fail(_SENSE_MEDIUM_ERROR, _ASC_UNRECOVERED_READ_ERROR)
            return

        self._lba += n
        self._blocks_left -= n
        self._residue -= n * self._block_size
        self.submit_xfer(self.ep_in, buf, self._read_chunk_done)

    def _read_chunk_done(self, ep, res, n):
        if res != 0:
            self._status = _CSW_STATUS_FAIL
            self._send_csw()
            return
        self._send_next_read_chunk()

    def _start_write10(self, cb):
        lba, blocks = self._parse_rw10(cb)
        if lba + blocks > self._block_count:
            self._fail(_SENSE_ILLEGAL_REQUEST, _ASC_LBA_OUT_OF_RANGE)
            return
        self._lba = lba
        self._blocks_left = blocks
        self._state = _ST_DATA_OUT
        self._recv_next_write_chunk()

    def _recv_next_write_chunk(self):
        if self._blocks_left <= 0:
            self._status = _CSW_STATUS_OK
            self._residue = 0
            self._send_csw()
            return

        n = min(self._blocks_left, _XFER_BLOCKS)
        self._pending_write_blocks = n
        buf = memoryview(self._xfer_buf)[: n * self._block_size]
        self.submit_xfer(self.ep_out, buf, self._write_chunk_done)

    def _write_chunk_done(self, ep, res, n):
        if res != 0:
            self._status = _CSW_STATUS_FAIL
            self._send_csw()
            return

        nblocks = self._pending_write_blocks
        buf = memoryview(self._xfer_buf)[: nblocks * self._block_size]
        try:
            self.bd.writeblocks(self._lba, buf)
        except OSError:
            self._fail(_SENSE_MEDIUM_ERROR, _ASC_WRITE_FAULT)
            return

        self._lba += nblocks
        self._blocks_left -= nblocks
        self._residue -= nblocks * self._block_size
        self._recv_next_write_chunk()
