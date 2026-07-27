#
# MicroPython FTP Client that mounts it's content as a VFS
# supports streaming to some extent
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import socket
import os

try:
    import uio as io
except ImportError:
    import io
import uctypes

# Stream ioctl request codes (see MicroPython py/stream.h)
_MP_STREAM_FLUSH = 1
_MP_STREAM_SEEK  = 2
_MP_STREAM_POLL  = 3
_MP_STREAM_CLOSE = 4
_SEEK_STRUCT = {
    "offset": 0 | uctypes.INT32,
    "whence": 4 | uctypes.INT32,
}

class FTPFile(io.IOBase):
    """ A file-like object implementing MicroPython's native stream
    protocol (io.IOBase), backed by either a local in-memory buffer or a
    live FTP data connection, depending on file size:

    - Read mode: if the file's size is known and under STREAM_THRESHOLD,
      it's downloaded fully up front (buffered) so reads/seeks are just
      local indexing. Otherwise (large or unknown size) it streams
      directly from the data connection; seeking re-opens the connection
      with REST at the target offset, since FTP data connections can't
      be seeked in place.
    - Write mode: starts buffered. If the buffer crosses STREAM_THRESHOLD,
      it's "promoted" to a live STOR connection - the buffered bytes are
      flushed and further writes stream directly. Once streaming, seeking
      is no longer possible (STOR doesn't support arbitrary-offset writes). """

    STREAM_THRESHOLD = 1024 * 1024  # 1MB

    def __init__(self, ftp, mode, file_path, size=None):
        self.ftp = ftp
        self.mode = mode
        self.file_path = file_path
        self.pos = 0
        self._closed = False
        self._streaming = False
        self._eof = False
        self.data_sock = None
        self.buf = None

        threshold = getattr(ftp, 'stream_threshold', self.STREAM_THRESHOLD)

        if 'r' in mode:
            if size is None or size >= threshold:
                self._start_read_stream(0)
            else:
                self.buf = self.ftp._retr_all(file_path)
        elif 'w' in mode:
            self.buf = bytearray()
        else:
            raise ValueError(f"Unsupported mode: {mode}")

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
        return False

    # -- streaming plumbing ------------------------------------------------

    def _start_read_stream(self, offset):
        """ Opens (or re-opens at `offset`) a passive RETR connection. """
        self.data_sock = self.ftp._pasv()
        if offset > 0:
            self.ftp._send_cmd(f"REST {offset}", 350)
        self.ftp._send_cmd(f"RETR {self.file_path}", 150)
        self._streaming = True
        self.pos = offset
        self._eof = False

    def _promote_write_to_stream(self):
        """ Switches a buffered write-mode file to a live STOR connection
        once its size crosses the threshold, flushing what's buffered. """
        self.data_sock = self.ftp._pasv()
        self.ftp._send_cmd(f"STOR {self.file_path}", 150)
        if self.buf:
            self.data_sock.write(bytes(self.buf))
        self.buf = None
        self._streaming = True

    def _abort_stream(self):
        """ Cleanly aborts an in-progress data transfer per RFC 959 so the
        control connection stays in sync for the next command. """
        try:
            self.ftp.sock.send(b"ABOR\r\n")
        except:
            pass
        try:
            self.data_sock.close()
        except:
            pass
        try:
            for _ in range(2):
                resp = self.ftp.sock_file.readline()
                if not resp or resp[:3] in (b'226', b'225'):
                    break
        except:
            pass

    def _finish_stream(self):
        """ Closes out a completed streaming transfer (STOR or a RETR read
        to EOF), consuming the final 226 response. """
        try:
            self.data_sock.close()
        except:
            pass
        try:
            resp = self.ftp.sock_file.readline()
            if not resp.startswith(b'226'):
                print("FTP Warning: Expected 226 after transfer, got:", resp)
        except:
            pass

    # -- stream protocol -----------------------------------------------

    def readinto(self, buf):
        if 'r' not in self.mode:
            return -1
        if self._streaming:
            try:
                n = self.data_sock.readinto(buf)
            except AttributeError:
                data = self.data_sock.read(len(buf))
                n = len(data) if data else 0
                if n:
                    buf[:n] = data
            if not n:
                self._eof = True
                return 0
            self.pos += n
            return n
        n = min(len(buf), len(self.buf) - self.pos)
        if n <= 0:
            return 0
        buf[:n] = self.buf[self.pos:self.pos + n]
        self.pos += n
        return n

    def read(self, size=-1):
        if 'r' not in self.mode:
            raise OSError("file not opened for reading")
        if self._streaming:
            data = self.data_sock.read() if size == -1 else self.data_sock.read(size)
            if not data:
                self._eof = True
            else:
                self.pos += len(data)
            return data
        if size == -1 or self.pos + size > len(self.buf):
            data = self.buf[self.pos:]
        else:
            data = self.buf[self.pos:self.pos + size]
        self.pos += len(data)
        return data

    def write(self, data):
        if 'w' not in self.mode:
            return -1
        if type(data) == str:
            data = data.encode('utf-8')

        threshold = getattr(self.ftp, 'stream_threshold', self.STREAM_THRESHOLD)
        if not self._streaming and len(self.buf) + len(data) >= threshold:
            self._promote_write_to_stream()

        if self._streaming:
            n = self.data_sock.write(data)
            if n:
                self.pos += n
            return n

        end = self.pos + len(data)
        if end > len(self.buf):
            self.buf.extend(bytes(end - len(self.buf)))
        self.buf[self.pos:end] = data
        self.pos = end
        return len(data)

    def tell(self):
        return self.pos

    def seek(self, offset, whence=0):
        """ Python-level convenience seek. Native callers don't hit this,
        they go through ioctl() below instead. """
        if 'w' in self.mode:
            if self._streaming:
                raise OSError("seek not supported once a large write has started streaming")
            if whence == 0:
                target = offset
            elif whence == 1:
                target = self.pos + offset
            elif whence == 2:
                target = len(self.buf) + offset
            else:
                raise ValueError("invalid whence")
            if target < 0:
                raise ValueError("negative seek position")
            self.pos = target
            return self.pos

        if whence == 0:
            target = offset
        elif whence == 1:
            target = self.pos + offset
        elif whence == 2:
            size = self.ftp._get_size(self.file_path) if self._streaming else len(self.buf)
            target = size + offset
        else:
            raise ValueError("invalid whence")
        if target < 0:
            raise ValueError("negative seek position")

        if self._streaming:
            if target == self.pos and not self._eof:
                return self.pos
            self._abort_stream()
            self._start_read_stream(target)
            return self.pos

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
        if 'w' in self.mode:
            if self._streaming:
                self._finish_stream()
            else:
                self.ftp._store_all(self.file_path, self.buf)
        elif self._streaming:
            if not self._eof:
                self._abort_stream()
            else:
                self._finish_stream()

class FTP_VFS:
    def __init__(self, host, port=21, user="anonymous", password="", stream_threshold=1024 * 1024):
        self.host = host
        self.port = port
        self.user = user
        self.password = password
        self.cwd = "/"
        self.sock = None
        self.sock_file = None
        self._stat_cache = {}
        self.stream_threshold = stream_threshold

    def _send_cmd(self, cmd, expected_code=None, _retry=True):
        """ Sends a command and returns the string response.

        If the control connection has actually dropped -- a socket
        error, the peer closing it, or a 421 ("Service not available,
        closing control connection", which servers commonly send for an
        idle timeout just before hanging up) -- transparently reconnects
        and retries the command once. Any other FTP error response (the
        server replied, just not with expected_code) is NOT treated as a
        dropped connection -- retrying that wouldn't help and would mask
        a real error like a genuine 550 File Not Found, so it's raised
        immediately instead. """
        try:
            self.sock.send((cmd + "\r\n").encode('utf-8'))
            resp = self.sock_file.readline()
            if not resp:
                raise OSError("connection closed by server")
            resp = resp.decode('utf-8').strip()
            while '-' in resp[:4]:
                resp = self.sock_file.readline().decode('utf-8').strip()
        except OSError as e:
            return self._reconnect_and_retry(cmd, expected_code, _retry,
                                             f"connection lost ({e})")

        if resp.startswith("421") and expected_code != 421:
            return self._reconnect_and_retry(cmd, expected_code, _retry,
                                             f"server closed connection ({resp})")

        if expected_code and not resp.startswith(str(expected_code)):
            raise OSError(f"FTP Error: Expected {expected_code}, got {resp}")
        return resp

    def _reconnect_and_retry(self, cmd, expected_code, _retry, reason):
        if not _retry or self.sock is None:
            raise OSError(reason)
        self._connect()
        return self._send_cmd(cmd, expected_code, _retry=False)

    def _pasv(self):
        """ Requests a passive data connection and returns a connected socket """
        resp = self._send_cmd("PASV", 227)
        start = resp.find('(') + 1
        end = resp.find(')')
        parts = resp[start:end].split(',')

        pasv_ip = ".".join(parts[0:4])
        pasv_port = (int(parts[4]) << 8) + int(parts[5])

        data_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        addr = socket.getaddrinfo(pasv_ip, pasv_port)[0][-1]
        data_sock.connect(addr)
        return data_sock

    def _connect(self):
        """ (Re)establishes the control connection and logs in. Used both
        for the initial mount() and to transparently recover a dropped
        connection from _send_cmd(). Internal commands here use
        _retry=False: if login fails right after a fresh connect, that's
        a real auth/protocol problem, not a stale-connection issue worth
        reconnecting-and-retrying again over. """
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass

        addr = socket.getaddrinfo(self.host, self.port)[0][-1]
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect(addr)
        self.sock_file = self.sock.makefile('rb')

        resp = self.sock_file.readline().decode('utf-8')
        while '-' in resp[:4]:
            resp = self.sock_file.readline().decode('utf-8')

        self._send_cmd(f"USER {self.user}", 331, _retry=False)
        self._send_cmd(f"PASS {self.password}", 230, _retry=False)
        # Binary mode: essential for correct byte-for-byte transfer of
        # non-text files, and SIZE/REST are only well-defined in this mode.
        self._send_cmd("TYPE I", 200, _retry=False)

        # A reconnect resets the server's idea of our working directory.
        if self.cwd not in ("", "/"):
            self._send_cmd(f"CWD {self.cwd}", 250, _retry=False)

    def mount(self, readonly, mkfs):
        self._connect()

    def umount(self):
        try:
            self._send_cmd("QUIT")
        except:
            pass
        if self.sock:
            self.sock.close()

    def chdir(self, dir_name):
        self._send_cmd(f"CWD {dir_name}", 250)
        self.cwd = dir_name

    def getcwd(self):
        resp = self._send_cmd("PWD", 257)
        return resp.split('"')[1]

    def ilistdir(self, path):
        if path == "": path = "."

        self._stat_cache.clear()

        data_sock = self._pasv()
        self._send_cmd(f"LIST {path}", 150)

        data_file = data_sock.makefile('rb')
        while True:
            line = data_file.readline()
            if not line:
                break
            line = line.decode('utf-8').strip()
            if not line:
                continue

            parts = line.split()
            name = " ".join(parts[8:]) 
            size = int(parts[4])
            is_dir = line.startswith('d')
            file_type = 0x4000 if is_dir else 0x8000

            clean_path = "" if path == "." else path
            cache_key = f"{clean_path}/{name}".strip("/")
            self._stat_cache[cache_key] = (file_type, size)

            yield (name, file_type, 0, size)

        data_sock.close()
        self.sock_file.readline()

    def open(self, file_path, mode):
        if 'r' in mode:
            size = self._lookup_size(file_path)
            return FTPFile(self, mode, file_path, size=size)
        elif 'w' in mode:
            return FTPFile(self, mode, file_path)
        else:
            raise ValueError(f"Unsupported mode: {mode}")

    def _lookup_size(self, file_path):
        """ Returns the file's size if known (from a prior ilistdir(), or
        via the FTP SIZE command), else None if it can't be determined -
        in which case FTPFile treats it as "unknown, assume large". """
        clean_path = file_path.strip("/")
        if clean_path in self._stat_cache:
            _, size = self._stat_cache[clean_path]
            return size
        try:
            return self._get_size(file_path)
        except OSError:
            return None

    def _get_size(self, path):
        resp = self._send_cmd(f"SIZE {path}", 213)
        return int(resp[4:].strip())

    def _retr_all(self, file_path):
        """ Downloads a file fully into memory and returns it as bytes. """
        data_sock = self._pasv()
        self._send_cmd(f"RETR {file_path}", 150)
        chunks = []
        while True:
            chunk = data_sock.read(1024)
            if not chunk:
                break
            chunks.append(chunk)
        data_sock.close()
        resp = self.sock_file.readline()
        if not resp.startswith(b'226'):
            print("FTP Warning: Expected 226 after transfer, got:", resp)
        return b''.join(chunks)

    def _store_all(self, file_path, data):
        """ Uploads an in-memory buffer as the full contents of a file. """
        data_sock = self._pasv()
        self._send_cmd(f"STOR {file_path}", 150)
        data_sock.write(bytes(data))
        data_sock.close()
        resp = self.sock_file.readline()
        if not resp.startswith(b'226'):
            print("FTP Warning: Expected 226 after transfer, got:", resp)

    def remove(self, path):
        self._send_cmd(f"DELE {path}", 250)

    def mkdir(self, path):
        self._send_cmd(f"MKD {path}", 257)

    def rmdir(self, path):
        self._send_cmd(f"RMD {path}", 250)

    def rename(self, old_path, new_path):
        self._send_cmd(f"RNFR {old_path}", 350)
        self._send_cmd(f"RNTO {new_path}", 250)
        self._stat_cache.clear()

    def stat(self, path):
        if path == "" or path == "/":
            return (0x4000, 0, 0, 0, 0, 0, 0, 0, 0, 0)

        clean_path = path.strip("/")
        if clean_path in self._stat_cache:
            mode, size = self._stat_cache[clean_path]
            return (mode, 0, 0, 0, 0, 0, size, 0, 0, 0)

        return (0x8000, 0, 0, 0, 0, 0, 0, 0, 0, 0)

def main(env, args):
    mount = args[0] if len(args) > 0 else None
    addr = args[1] if len(args) > 1 else None
    port = int(args[2]) if len(args) > 2 else 21
    auth_user = args[3] if len(args) > 3 else "anonymous"
    auth_pass = args[4] if len(args) > 4 else ""

    if mount is None or addr is None:
        print("Usage: ftp <mount> <host> [port] [user] [password]")
        return

    ftp_mount = FTP_VFS(addr, port, auth_user, auth_pass)
    os.mount(ftp_mount, "/" + mount)
    print(f"FTP {addr}:{port} successfully mounted at /{mount}")
