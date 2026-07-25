#
# MicroPython SFTP server (wolfSSH-backed)
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import os
import network
import modsftpd
import modsshd

_HOST_KEY_FILE = "/flash/.sshd_host_key"  # shared with sshd.py -- same device, one host identity

_server = None
_env = None

_next_handle = 1
_open_files = {}  # handle -> file object
_open_dirs = {}   # handle -> iterator from os.ilistdir()

def get_ip():
    sta = network.WLAN(network.STA_IF)
    if sta.active():
        return sta.ifconfig()[0]
    ap = network.WLAN(network.AP_IF)
    if ap.active():
        return ap.ifconfig()[0]
    return "127.0.0.1"

def _load_host_key():
    try:
        with open(_HOST_KEY_FILE, "rb") as f:
            return f.read()
    except OSError:
        key = modsshd.make_host_key()
        with open(_HOST_KEY_FILE, "wb") as f:
            f.write(key)
        return key

def _on_event(code):
    if _server is None:
        return
    slot = abs(code) - 1
    connected = code > 0
    ip = _server.session_ip(slot)
    if _env is None or _env.sts is None:
        return
    state = "connected" if connected else "disconnected"
    _env.sts.notify(f"sftpd: {ip} {state}")

def _alloc_handle():
    global _next_handle
    h = _next_handle
    _next_handle += 1
    return h

def _mode_str(flags, path):
    if flags & modsftpd.FXF_RDWR:
        return "r+b"
    if flags & modsftpd.FXF_WRITE:
        if flags & modsftpd.FXF_APPEND:
            return "ab"
        if flags & modsftpd.FXF_TRUNC:
            return "wb"
        try:
            os.stat(path)
            return "r+b"
        except OSError:
            return "wb"
    return "rb"

_OP_NAMES = {
    modsftpd.OP_STAT: "STAT", modsftpd.OP_LSTAT: "LSTAT",
    modsftpd.OP_OPEN: "OPEN", modsftpd.OP_READ: "READ",
    modsftpd.OP_WRITE: "WRITE", modsftpd.OP_CLOSE: "CLOSE",
    modsftpd.OP_OPENDIR: "OPENDIR", modsftpd.OP_READDIR: "READDIR",
    modsftpd.OP_CLOSEDIR: "CLOSEDIR", modsftpd.OP_MKDIR: "MKDIR",
    modsftpd.OP_RMDIR: "RMDIR", modsftpd.OP_REMOVE: "REMOVE",
    modsftpd.OP_RENAME: "RENAME", modsftpd.OP_GETCWD: "GETCWD",
}

def _on_vfs_request(server):
    op, path_a, path_b, flags, handle, offset, data = server.request()
    ret = 0
    resp_handle = -1
    resp_data = b""
    mode = size = mtime = 0
    name = ""
    eof = False

    try:
        if op in (modsftpd.OP_STAT, modsftpd.OP_LSTAT):
            st = os.stat(path_a)
            is_dir = (st[0] & 0x4000) != 0
            mode = (st[0] & ~0o777) | (0o755 if is_dir else 0o644)
            size, mtime = st[6], st[8]

        elif op == modsftpd.OP_OPEN:
            f = open(path_a, _mode_str(flags, path_a))
            resp_handle = _alloc_handle()
            _open_files[resp_handle] = f

        elif op == modsftpd.OP_READ:
            f = _open_files[handle]
            if f.tell() != offset:
                f.seek(offset)
            resp_data = f.read(modsftpd.SCRATCH_SZ)

        elif op == modsftpd.OP_WRITE:
            f = _open_files[handle]
            if f.tell() != offset:
                f.seek(offset)
            n = f.write(data)
            size = n if n is not None else len(data)

        elif op == modsftpd.OP_CLOSE:
            f = _open_files.pop(handle, None)
            if f is not None:
                f.close()

        elif op == modsftpd.OP_OPENDIR:
            resp_handle = _alloc_handle()
            _open_dirs[resp_handle] = os.ilistdir(path_a)

        elif op == modsftpd.OP_READDIR:
            it = _open_dirs.get(handle)
            entry = next(it, None) if it is not None else None
            if entry is None:
                eof = True
            else:
                name = entry[0]

        elif op == modsftpd.OP_CLOSEDIR:
            _open_dirs.pop(handle, None)

        elif op == modsftpd.OP_MKDIR:
            os.mkdir(path_a)

        elif op == modsftpd.OP_RMDIR:
            os.rmdir(path_a)

        elif op == modsftpd.OP_REMOVE:
            os.remove(path_a)

        elif op == modsftpd.OP_RENAME:
            os.rename(path_a, path_b)

        elif op == modsftpd.OP_GETCWD:
            name = os.getcwd()

        else:
            ret = -1

    except BaseException as e:
        ret = -1
        #print(f"sftpd: {_OP_NAMES.get(op, op)} '{path_a}' failed: {e}")

    server.respond(ret, resp_handle, resp_data, mode, size, mtime, name, eof)

def main(env, args):
    global _server, _env

    _env = env
    auth_user = args[0] if len(args) > 0 else None
    auth_pass = args[1] if len(args) > 1 else None
    port = int(args[2]) if len(args) > 2 else 2222

    if args and args[0] == "stop":
        if _server:
            _server.stop()
            print("sftpd stopping...")
        else:
            print("sftpd not running")
        return

    if auth_user is None or auth_pass is None:
        print("Usage: sftpd <user> <password> [port]\n        sftpd stop")
        return

    if _server is not None and _server.status() != modsftpd.STOPPED:
        print("sftpd: already running\nuse 'sftpd stop' to stop it")
        return

    host_key = _load_host_key()
    _server = modsftpd.Server(auth_user, auth_pass, host_key, port)
    _server.set_notify(_on_event)
    _server.set_vfs_service(_on_vfs_request)
    _server.start()

    print(f"sftpd: listening on {get_ip()}:{port}")
