#
# MicroPython SSH server (wolfSSH-backed)
# Copyright (c) 2026 8bitmcu
# License: MIT
#
# Shares the shell already running on this device with a
# remote SSH client rather than spawning an isolated one

import network
import modsshd

_HOST_KEY_FILE = "/flash/.sshd_host_key"

_server = None
_env = None

def _on_event(connected):
    if _server is None or _env is None or _env.sts is None:
        return
    ip = _server.last_client_ip()
    state = "connected" if connected else "disconnected"
    _env.sts.notify(f"sshd: {ip} {state}")

def get_ip():
    """Retrieve the active IP address of the device."""
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

def main(env, args):
    global _server, _env

    _env = env
    auth_user = args[0] if len(args) > 0 else None
    auth_pass = args[1] if len(args) > 1 else None
    port = int(args[2]) if len(args) > 2 else 22

    if args and args[0] == "stop":
        if _server:
            _server.stop()
            print("sshd stopping...")
        else:
            print("sshd not running")
        return

    if auth_user is None or auth_pass is None:
        print("Usage: sshd <user> <password> [port]\n       sshd stop")
        return

    if _server is not None and _server.status() != modsshd.STOPPED:
        print("sshd: already running\nuse 'sshd stop' to stop it")
        return

    host_key = _load_host_key()
    _server = modsshd.Server(env.kvm, auth_user, auth_pass, host_key, port)
    _server.set_notify(_on_event)
    _server.start()

    print(f"sshd: listening on {get_ip()}:{port}")
