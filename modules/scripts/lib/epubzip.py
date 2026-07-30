#
# Minimal read-only ZIP container reader for EPUB files
# Copyright (c) 2026 8bitmcu
# License: MIT
#
# EPUB archives are simple, single-part ZIPs -- no zip64, no encryption,
# no multi-disk spanning -- so this only implements what that needs:
# locate the End Of Central Directory record, walk the central directory
# to build a name -> entry index, then read individual entries by seeking
# straight to their local file header. Central directory sizes are
# authoritative (some writers zero out the local header's own size
# fields and rely on a trailing data descriptor instead), so entry
# metadata always comes from the central directory, never the local
# header.
#

import struct
import io
import deflate

_EOCD_SIG = b"PK\x05\x06"
_CDFH_SIG = b"PK\x01\x02"
_LFH_SIG = b"PK\x03\x04"

_EOCD_STRUCT = "<4sHHHHIIH"
_EOCD_SIZE = struct.calcsize(_EOCD_STRUCT)  # 22

_CDFH_STRUCT = "<4sHHHHHHIIIHHHHHII"
_CDFH_SIZE = struct.calcsize(_CDFH_STRUCT)  # 46

_LFH_STRUCT = "<4sHHHHHIIIHH"
_LFH_SIZE = struct.calcsize(_LFH_STRUCT)  # 30

# A ZIP comment can be up to 65535 bytes, so the EOCD record can start
# up to that far before the end of the file.
_EOCD_SEARCH_WINDOW = _EOCD_SIZE + 0xFFFF


class EpubZipError(Exception):
    pass


class ZipReader:
    """ Read-only view over a ZIP file, opened for random access on
    whatever filesystem it lives on (SD card, typically -- these files
    are too big to hold in RAM whole). """

    def __init__(self, path):
        self._f = open(path, "rb")
        self._entries = {}  # name -> (local_header_offset, comp_size, uncomp_size, method)
        self._read_central_directory()

    def close(self):
        self._f.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def namelist(self):
        return list(self._entries.keys())

    def _read_central_directory(self):
        f = self._f
        f.seek(0, 2)  # SEEK_END
        file_size = f.tell()

        window = min(file_size, _EOCD_SEARCH_WINDOW)
        f.seek(file_size - window)
        tail = f.read(window)

        idx = tail.rfind(_EOCD_SIG)
        if idx < 0:
            raise EpubZipError("not a zip file (no end-of-central-directory record)")

        eocd = struct.unpack(_EOCD_STRUCT, tail[idx:idx + _EOCD_SIZE])
        _, _, _, _, entry_count, cd_size, cd_offset, _ = eocd

        f.seek(cd_offset)
        cd_data = f.read(cd_size)

        pos = 0
        for _ in range(entry_count):
            if cd_data[pos:pos + 4] != _CDFH_SIG:
                raise EpubZipError("corrupt central directory")
            fields = struct.unpack(_CDFH_STRUCT, cd_data[pos:pos + _CDFH_SIZE])
            method = fields[4]
            comp_size = fields[8]
            uncomp_size = fields[9]
            fname_len = fields[10]
            extra_len = fields[11]
            comment_len = fields[12]
            local_offset = fields[16]

            name_start = pos + _CDFH_SIZE
            name = cd_data[name_start:name_start + fname_len].decode("utf-8")

            self._entries[name] = (local_offset, comp_size, uncomp_size, method)

            pos = name_start + fname_len + extra_len + comment_len

    def read(self, name):
        """ Returns the fully decompressed bytes of `name`. """
        entry = self._entries.get(name)
        if entry is None:
            raise KeyError(name)
        local_offset, comp_size, uncomp_size, method = entry

        f = self._f
        f.seek(local_offset)
        lfh = f.read(_LFH_SIZE)
        if lfh[:4] != _LFH_SIG:
            raise EpubZipError("corrupt local file header for %r" % name)
        fname_len, extra_len = struct.unpack("<HH", lfh[26:30])

        f.seek(local_offset + _LFH_SIZE + fname_len + extra_len)
        data = f.read(comp_size)

        if method == 0:  # STORED
            if len(data) != uncomp_size:
                raise EpubZipError("%r is truncated" % name)
            return data
        elif method == 8:  # DEFLATE
            stream = deflate.DeflateIO(io.BytesIO(data), deflate.RAW, 15)
            out = stream.read()
            if len(out) != uncomp_size:
                raise EpubZipError("%r decompressed to the wrong size" % name)
            return out
        else:
            raise EpubZipError("unsupported compression method %d for %r" % (method, name))
