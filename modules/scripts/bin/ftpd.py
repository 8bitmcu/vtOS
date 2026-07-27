#
# MicroPython Asyncio FTP Server
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import os
import sys
import select
import network
import asyncio

DATA_PORT_START = 2000
DATA_PORT_END = 2050
SERVER_IP = "0.0.0.0"

def get_ip():
    """Retrieve the active IP address of the device."""
    sta = network.WLAN(network.STA_IF)
    if sta.active():
        return sta.ifconfig()[0]
    ap = network.WLAN(network.AP_IF)
    if ap.active():
        return ap.ifconfig()[0]
    return "127.0.0.1"

class FTPConnection:
    def __init__(self, reader, writer, client_ip, auth_user, auth_pass):
        self.reader = reader
        self.writer = writer
        self.client_ip = client_ip
        self.auth_user = auth_user
        self.auth_pass = auth_pass
        self.cwd = "/"
        self.authenticated = False  # New flag
        self.username = None
        self.data_port = DATA_PORT_START
        self.data_server = None
        self.data_reader = None
        self.data_writer = None
        self.pasv_task = None
        self.rename_from = None

    async def send_resp(self, code, message):
        response = f"{code} {message}\r\n"
        self.writer.write(response.encode('utf-8'))
        await self.writer.drain()

    async def handle_data_client(self, reader, writer):
        """Callback for when the client connects to the passive data port."""
        self.data_reader = reader
        self.data_writer = writer

    async def open_datachannel(self):
        """Sets up the passive listening socket and returns the PASV response."""
        global DATA_PORT_START
        self.data_port = DATA_PORT_START
        DATA_PORT_START += 1
        if DATA_PORT_START > DATA_PORT_END:
            DATA_PORT_START = 2000

        if self.data_server:
            self.data_server.close()
            await self.data_server.wait_closed()

        self.data_server = await asyncio.start_server(self.handle_data_client, "0.0.0.0", self.data_port)

        # Format IP and Port for PASV response (h1,h2,h3,h4,p1,p2)
        ip_parts = SERVER_IP.split('.')
        p1 = self.data_port >> 8
        p2 = self.data_port & 0xFF
        pasv_str = f"{ip_parts[0]},{ip_parts[1]},{ip_parts[2]},{ip_parts[3]},{p1},{p2}"

        await self.send_resp(227, f"Entering Passive Mode ({pasv_str}).")

    async def wait_for_data_client(self, timeout=10):
        """Wait for the client to connect to the open PASV port."""
        for _ in range(timeout * 10):
            if self.data_writer is not None:
                return True
            await asyncio.sleep_ms(100)
        return False

    async def close_datachannel(self):
        if self.data_writer:
            self.data_writer.close()
            await self.data_writer.wait_closed()
            self.data_writer = None
            self.data_reader = None
        if self.data_server:
            self.data_server.close()
            await self.data_server.wait_closed()
            self.data_server = None

    def resolve_path(self, path, base=None):
        if path.startswith("/"):
            return path
        if base is None:
            base = self.cwd
        if base == "/":
            return "/" + path
        return base + "/" + path

    async def run(self):
        await self.send_resp(220, "FTP Server Ready.")

        try:
            while True:
                line = await self.reader.readline()
                if not line:
                    break

                cmd_line = line.decode('utf-8').strip()
                if not cmd_line:
                    continue

                parts = cmd_line.split(" ", 1)
                cmd = parts[0].upper()
                payload = parts[1] if len(parts) > 1 else ""

                print(f"{self.client_ip}: {cmd} {payload}")

                if not self.authenticated:
                    if cmd == "USER":
                        self.username = payload
                        await self.send_resp(331, "Password required.")

                    elif cmd == "PASS":
                        if self.username == self.auth_user and payload == self.auth_pass:
                            self.authenticated = True
                            await self.send_resp(230, "User logged in, proceed.")
                        else:
                            await self.send_resp(530, "Login incorrect.")
                            break

                    elif cmd == "QUIT":
                        await self.send_resp(221, "Goodbye.")
                        break

                    else:
                        await self.send_resp(530, "Please login with USER and PASS.")

                    continue

                elif cmd == "SYST":
                    await self.send_resp(215, "UNIX Type: L8")

                elif cmd == "TYPE":
                    await self.send_resp(200, "Type set to binary.")

                elif cmd == "PWD":
                    await self.send_resp(257, f'"{self.cwd}" is current directory.')

                elif cmd == "CWD":
                    target = self.resolve_path(payload)
                    try:
                        os.listdir(target) # Check if it exists
                        self.cwd = target
                        await self.send_resp(250, "CWD command successful.")
                    except OSError:
                        await self.send_resp(550, "No such directory.")

                elif cmd == "CDUP":
                    self.cwd = "/" if self.cwd.count("/") <= 1 else self.cwd.rsplit("/", 1)[0]
                    await self.send_resp(250, "CDUP command successful.")

                elif cmd == "SIZE":
                    target = self.resolve_path(payload)
                    try:
                        size = os.stat(target)[6]
                        await self.send_resp(213, str(size))
                    except OSError:
                        await self.send_resp(550, "File not found.")

                elif cmd == "STAT":
                    # Simple version: return info for target, or directory listing
                    target = self.resolve_path(payload or self.cwd)
                    try:
                        stat = os.stat(target)
                        # Minimal STAT response
                        await self.send_resp(213, f"Size: {stat[6]}")
                    except OSError:
                        await self.send_resp(550, "Status failed.")

                elif cmd == "RNFR":
                    target = self.resolve_path(payload)
                    try:
                        os.stat(target) # Check existence
                        self.rename_from = target
                        await self.send_resp(350, "File exists, ready for destination.")
                    except OSError:
                        await self.send_resp(550, "File not found.")

                elif cmd == "RNTO":
                    if self.rename_from:
                        target = self.resolve_path(payload)
                        try:
                            os.rename(self.rename_from, target)
                            self.rename_from = None
                            await self.send_resp(250, "Rename successful.")
                        except OSError:
                            await self.send_resp(550, "Rename failed.")
                    else:
                        await self.send_resp(503, "Bad sequence: send RNFR first.")

                elif cmd == "MKD":
                    target = self.resolve_path(payload)
                    try:
                        os.mkdir(target)
                        await self.send_resp(257, f'"{payload}" directory created.')
                    except OSError:
                        await self.send_resp(550, "Failed to create directory.")

                elif cmd in ("DELE", "DELETE"):
                    target = self.resolve_path(payload)
                    try:
                        os.remove(target)
                        await self.send_resp(250, "File deleted successfully.")
                    except OSError:
                        await self.send_resp(550, "Failed to delete file or file not found.")

                elif cmd == "RMD":
                    target = self.resolve_path(payload)
                    try:
                        os.rmdir(target)
                        await self.send_resp(250, "Directory removed successfully.")
                    except OSError:
                        await self.send_resp(550, "Failed to remove directory or directory not empty.")

                elif cmd == "PASV":
                    await self.open_datachannel()

                elif cmd == "PORT":
                    await self.send_resp(502, "502 Active mode not supported, use PASV.")

                elif cmd == "LIST":
                    # LIST optionally takes a pathname argument (RFC 959)
                    # -- some clients browse by sending it directly
                    # instead of CWD-ing first, so this must respect it
                    # rather than always listing self.cwd.
                    target = self.resolve_path(payload) if payload else self.cwd
                    await self.send_resp(150, "Opening ASCII mode data connection for file list.")
                    if await self.wait_for_data_client():
                        try:
                            for item in os.listdir(target):
                                stat = os.stat(self.resolve_path(item, base=target))
                                is_dir = (stat[0] & 0x4000) != 0
                                size = stat[6]
                                # Minimal Unix-like listing
                                perms = "drwxr-xr-x" if is_dir else "-rw-r--r--"
                                list_line = f"{perms} 1 owner group {size:>8} Jan 01 00:00 {item}\r\n"
                                self.data_writer.write(list_line.encode('utf-8'))
                                await self.data_writer.drain()
                                await asyncio.sleep(0) # Yield control
                            await self.close_datachannel()
                            await self.send_resp(226, "Transfer complete.")
                        except OSError:
                            await self.close_datachannel()
                            await self.send_resp(550, "Failed to list directory.")
                    else:
                        await self.send_resp(425, "Can't open data connection.")

                elif cmd == "RETR":
                    target = self.resolve_path(payload)
                    await self.send_resp(150, "Opening binary mode data connection.")
                    if await self.wait_for_data_client():
                        try:
                            with open(target, "rb") as f:
                                while True:
                                    chunk = f.read(512)
                                    if not chunk:
                                        break
                                    self.data_writer.write(chunk)
                                    await self.data_writer.drain()
                                    # CRITICAL: Yield to event loop to keep UI/other clients alive
                                    await asyncio.sleep(0) 
                            await self.close_datachannel()
                            await self.send_resp(226, "Transfer complete.")
                        except OSError:
                            await self.close_datachannel()
                            await self.send_resp(550, "File not found or access denied.")
                    else:
                        await self.send_resp(425, "Can't open data connection.")

                elif cmd == "STOR":
                    target = self.resolve_path(payload)
                    await self.send_resp(150, "Opening binary mode data connection.")
                    if await self.wait_for_data_client():
                        try:
                            with open(target, "wb") as f:
                                while True:
                                    chunk = await self.data_reader.read(512)
                                    if not chunk:
                                        break
                                    f.write(chunk)
                                    await asyncio.sleep(0) # CRITICAL
                            await self.close_datachannel()
                            await self.send_resp(226, "Transfer complete.")
                        except OSError:
                            await self.close_datachannel()
                            await self.send_resp(550, "Failed to write file.")
                    else:
                        await self.send_resp(425, "Can't open data connection.")

                elif cmd == "QUIT":
                    await self.send_resp(221, "Goodbye.")
                    break

                else:
                    await self.send_resp(502, "Command not implemented.")

        except Exception as e:
            print(f"{self.client_ip} error: {e}")

        finally:
            await self.close_datachannel()
            self.writer.close()
            await self.writer.wait_closed()
            print(f"{self.client_ip}: Disconnected")

async def serve_ftp(reader, writer, auth_user, auth_pass):
    """Entry point for a new client connection."""
    client_ip = writer.get_extra_info('peername')[0]
    print(f"{client_ip}: New Connection")
    conn = FTPConnection(reader, writer, client_ip, auth_user, auth_pass)
    await conn.run()

async def watch_for_exit(loop):
    """Watches stdin for 'q' or ESC and stops the event loop when pressed."""
    while True:
        r, _, _ = select.select([sys.stdin], [], [], 0)
        if r:
            char = sys.stdin.read(1)
            if char in ('q', 'Q', '\x1b'):
                print("\r\nStopping FTP server...")
                loop.stop()
                return
        await asyncio.sleep_ms(100)

def main(env, args):
    port = int(args[0]) if len(args) > 0 else 21
    auth_user = args[1] if len(args) > 1 else "admin"
    auth_pass = args[2] if len(args) > 2 else "admin"

    async def factory(reader, writer):
        await serve_ftp(reader, writer, auth_user, auth_pass)

    global SERVER_IP
    SERVER_IP = get_ip()

    print(f"Starting FTP Server on {SERVER_IP}:{port}\nPress q or ESC to stop")
    loop = asyncio.get_event_loop()
    server = loop.run_until_complete(asyncio.start_server(factory, "0.0.0.0", port))
    loop.create_task(watch_for_exit(loop))
    loop.run_forever()

    server.close()
    loop.run_until_complete(server.wait_closed())
