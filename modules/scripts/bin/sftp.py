#
# MicroPython SFTP Client that mounts its content as a VFS
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import os
import time
import modsftp

try:
    import uio as io
except ImportError:
    import io
import uctypes

_CONNECT_TIMEOUT_MS = 15000
_READ_CHUNK = 4096  # matches modsftp's internal per-call cap

# Stream ioctl request codes (see MicroPython py/stream.h)
_MP_STREAM_FLUSH = 1
_MP_STREAM_SEEK  = 2
_MP_STREAM_POLL  = 3
_MP_STREAM_CLOSE = 4
_SEEK_STRUCT = {
    "offset": 0 | uctypes.INT32,
    "whence": 4 | uctypes.INT32,
}

class SFTPFile(io.IOBase):
    """ File-like object over a modsftp.Client handle. Unlike ftp.py's
    FTPFile, seeking needs no reconnect-and-REST hack -- every read/write
    already carries an explicit byte offset, so it's just self.pos
    bookkeeping. modsftp.Client.read()/write() cap each call at 4096
    bytes internally, so both read() and readinto() loop here rather
    than assuming one call satisfies the whole request. """

    def __init__(self, client, mode, file_path):
        self.client = client
        self.mode = mode
        self.file_path = file_path
        self.pos = 0
        self._closed = False

        if 'r' in mode:
            flags = modsftp.FXF_READ
        elif 'w' in mode:
            flags = modsftp.FXF_WRITE | modsftp.FXF_CREAT | modsftp.FXF_TRUNC
        else:
            raise ValueError(f"Unsupported mode: {mode}")

        self.handle = client.open(file_path, flags)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
        return False

    def readinto(self, buf):
        if 'r' not in self.mode:
            return -1
        total = 0
        want = len(buf)
        while total < want:
            chunk = self.client.read(self.handle, self.pos, want - total)
            n = len(chunk)
            if n == 0:
                break
            buf[total:total + n] = chunk
            self.pos += n
            total += n
        return total

    def read(self, size=-1):
        if 'r' not in self.mode:
            raise OSError("file not opened for reading")
        chunks = []
        remaining = size
        while remaining != 0:
            want = _READ_CHUNK if remaining < 0 else min(remaining, _READ_CHUNK)
            data = self.client.read(self.handle, self.pos, want)
            if not data:
                break
            chunks.append(data)
            self.pos += len(data)
            if remaining > 0:
                remaining -= len(data)
        return b"".join(chunks)

    def write(self, data):
        if 'w' not in self.mode:
            return -1
        if type(data) == str:
            data = data.encode('utf-8')

        mv = memoryview(data)
        total = 0
        while total < len(mv):
            chunk = mv[total:total + _READ_CHUNK]
            n = self.client.write(self.handle, self.pos, chunk)
            if not n:
                break
            self.pos += n
            total += n
        return total

    def tell(self):
        return self.pos

    def seek(self, offset, whence=0):
        if whence == 0:
            target = offset
        elif whence == 1:
            target = self.pos + offset
        elif whence == 2:
            if 'r' not in self.mode:
                raise OSError("seek from end not supported while writing")
            _, size, _ = self.client.stat(self.file_path)
            target = size + offset
        else:
            raise ValueError("invalid whence")
        if target < 0:
            raise ValueError("negative seek position")
        self.pos = target
        return self.pos

    def ioctl(self, req, arg):
        if req == _MP_STREAM_SEEK:
            try:
                s = uctypes.struct(arg, _SEEK_STRUCT, uctypes.NATIVE)
                self.pos = self.seek(s.offset, s.whence)
                s.offset = self.pos
                return 0
            except Exception:
                return -1
        if req == _MP_STREAM_CLOSE:
            self.close()
            return 0
        if req == _MP_STREAM_FLUSH:
            return 0
        if req == _MP_STREAM_POLL:
            return 0
        return -1

    def close(self):
        if self._closed:
            return
        self._closed = True
        try:
            self.client.close(self.handle)
        except OSError:
            pass

class SFTP_VFS:
    def __init__(self, host, port=22, user="", password=""):
        self.host = host
        self.port = port
        self.user = user
        self.password = password
        self.cwd = "/"
        self.client = None

    def _connect(self):
        self.client = modsftp.Client()
        self.client.connect(self.host, self.port, self.user, self.password)

        waited_ms = 0
        while self.client.status() == modsftp.CONNECTING:
            time.sleep_ms(10)
            waited_ms += 10
            if waited_ms >= _CONNECT_TIMEOUT_MS:
                self.client.disconnect()
                raise OSError("connect timed out")

        if self.client.status() != modsftp.CONNECTED:
            raise OSError(f"connect failed (error_code={self.client.error_code()})")

    def mount(self, readonly, mkfs):
        self._connect()

    def umount(self):
        if self.client:
            self.client.disconnect()

    def chdir(self, dir_name):
        # No stateful server-side cwd in this design (every call takes
        # a full path) -- purely client-side bookkeeping, same as what
        # getcwd() reports back. Mirrors ftp.py's shape; unlike ftp.py's
        # FTP control connection, there's no server round-trip to fail
        # here, so this can't itself raise.
        if dir_name.startswith("/"):
            self.cwd = dir_name
        elif self.cwd in ("", "/"):
            self.cwd = "/" + dir_name
        else:
            self.cwd = self.cwd + "/" + dir_name

    def getcwd(self):
        return self.cwd

    def ilistdir(self, path):
        # Mirrors ftp.py's own "" -> root normalization -- the VFS layer
        # calls this with an empty path for the mount root itself.
        full = path if path else "/"
        for name, (mode, size, mtime) in self.client.ls(full):
            file_type = mode & 0xC000  # 0x4000 dir / 0x8000 file
            yield (name, file_type, 0, size)

    def open(self, file_path, mode):
        return SFTPFile(self.client, mode, file_path)

    def remove(self, path):
        self.client.remove(path)

    def mkdir(self, path):
        self.client.mkdir(path)

    def rmdir(self, path):
        self.client.rmdir(path)

    def rename(self, old_path, new_path):
        self.client.rename(old_path, new_path)

    def stat(self, path):
        if path == "" or path == "/":
            return (0x4000, 0, 0, 0, 0, 0, 0, 0, 0, 0)
        mode, size, mtime = self.client.stat(path)
        return (mode, 0, 0, 0, 0, 0, size, 0, mtime, mtime)

def main(env, args):
    mount = args[0] if len(args) > 0 else None
    addr = args[1] if len(args) > 1 else None
    port = int(args[2]) if len(args) > 2 else 22
    auth_user = args[3] if len(args) > 3 else ""
    auth_pass = args[4] if len(args) > 4 else ""

    if mount is None or addr is None:
        print("Usage: sftp <mount> <host> [port] [user] [password]")
        return

    sftp_mount = SFTP_VFS(addr, port, auth_user, auth_pass)
    os.mount(sftp_mount, "/" + mount)
    print(f"SFTP {addr}:{port} successfully mounted at /{mount}")
