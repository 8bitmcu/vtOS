#
# MicroPython Telnet Client
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import select
import socket
import time

# --- Telnet Protocol Constants ---
IAC  = 255  # "Interpret As Command"
DONT = 254
DO   = 253
WONT = 252
WILL = 251
SB   = 250  # Sub-negotiation Begin
SE   = 240  # Sub-negotiation End

# Telnet Options
OPT_ECHO = 1
OPT_SGA  = 3   # Suppress Go Ahead
OPT_NAWS = 31  # Negotiate About Window Size

class TelnetClient:
    def __init__(self, env, *args):
        if len(args) == 1 and not isinstance(args[0], str):
            args = tuple(args[0])  # unpack tuple/list passed as single arg
        if not args:
            raise ValueError("Usage: telnet <host> [port]")

        self.host = args[0]

        if len(args) > 1:
            try:
                self.port = int(args[1])
            except ValueError:
                raise ValueError(f"Invalid port number: {args[1]}")
        else:
            self.port = 23

        self.env = env
        self.cols = env.cols
        self.rows = env.rows
        self.connected = False

        # State Machine Tracking
        self.buffer = b""
        self.partial_seq = b""
        self.telnet_command_mode = False
        self.current_option_cmd = None
        self.subnegotiation_buffer = bytearray()
        self.in_subnegotiation = False

        addr = socket.getaddrinfo(self.host, self.port)[0][-1]
        self.socket = socket.socket()
        self.socket.connect(addr)
        self.socket.setblocking(False)
        self.socket.send(bytes([IAC, WILL, OPT_SGA, IAC, WILL, OPT_ECHO, IAC, WILL, OPT_NAWS]))
        self.send_window_size()
        self.connected = True

    def send_window_size(self):
        """
        RFC 1073: Sends the terminal size to the server.
        Format: IAC SB NAWS <width_high> <width_low> <height_high> <height_low> IAC SE
        """
        # Pack width and height into 16-bit big-endian integers
        w_h, w_l = (self.cols >> 8) & 0xFF, self.cols & 0xFF
        h_h, h_l = (self.rows >> 8) & 0xFF, self.rows & 0xFF

        payload = bytes([IAC, SB, OPT_NAWS, w_h, w_l, h_h, h_l, IAC, SE])
        self.socket.send(payload)

    def run(self):
        try:
            self.process()
        except KeyboardInterrupt:
            self.close()

    def process(self):
        while self.connected:
            try:
                r, _, _ = select.select([self.socket], [], [], 0.01)
                if r:
                    new_data = self.socket.recv(2048)
                    if not new_data:
                        self.connected = False
                        return

                    # Combine any leftover bytes from last time with new data
                    data = self.partial_seq + new_data
                    self.partial_seq = b""

                    # Check if the data ends in the middle of a sequence
                    # \x1b is the start, \x1b[ is the common prefix
                    if data.endswith(b'\x1b'):
                        self.partial_seq = b'\x1b'
                        data = data[:-1]
                    elif data.endswith(b'\x1b['):
                        self.partial_seq = b'\x1b['
                        data = data[:-2]
                    elif data[-1:] in b'0123456789;[' and b'\x1b' in data[-8:]:
                        # Search backwards for the last ESC
                        last_esc = data.rfind(b'\x1b')
                        self.partial_seq = data[last_esc:]
                        data = data[:last_esc]

                    if IAC in data:
                        self._process_complex_data(data)
                    else:
                        self.env.kvm.write(data)

            except OSError:
                pass

            buf = bytearray(1)
            if self.env.kvm.readinto(buf):
                char_byte = buf[0]
                if char_byte == 13:
                    self.socket.send(b'\r\n')
                else:
                    self.socket.send(bytes([char_byte]))
            time.sleep_ms(10)

    def _process_complex_data(self, data):
        """
        Only called when the 'Fast Path' detects an IAC (255) byte.
        This handles the state machine for negotiations.
        """
        clean_data = bytearray()
        i = 0
        while i < len(data):
            byte = data[i]

            if not self.telnet_command_mode and byte != IAC:
                clean_data.append(byte)
                i += 1
            elif not self.telnet_command_mode and byte == IAC:
                self.telnet_command_mode = True
                i += 1
            elif self.telnet_command_mode:
                # Sub-negotiation logic
                if byte == SB:
                    self.in_subnegotiation = True
                    self.subnegotiation_buffer = bytearray()
                    i += 1
                elif self.in_subnegotiation:
                    if byte == SE:
                        self._handle_subnegotiation(self.subnegotiation_buffer)
                        self.in_subnegotiation = False
                        self.telnet_command_mode = False
                    else:
                        if byte != IAC: self.subnegotiation_buffer.append(byte)
                    i += 1
                # Simple Command logic (DO/DONT/WILL/WONT)
                elif byte in (251, 252, 253, 254): # WILL, WONT, DO, DONT
                    self.current_option_cmd = byte
                    i += 1
                elif self.current_option_cmd:
                    self._handle_negotiation(self.current_option_cmd, byte)
                    self.current_option_cmd = None
                    self.telnet_command_mode = False
                    i += 1
                else:
                    # Catch-all for other commands (like NOP)
                    self.telnet_command_mode = False
                    i += 1

        if clean_data:
            self.env.kvm.write(clean_data)


    def _handle_negotiation(self, command, option):
        """Decide how to reply to server requests"""
        response = bytearray()

        if command == DO and option == OPT_NAWS:
            # Server asks: "Do you support Window Size?"
            # We reply: "WILL" (Yes) and immediately send the size
            response.extend([IAC, WILL, OPT_NAWS])
            self.socket.send(response)
            self.send_window_size()
            return

        if command == DO and option == OPT_SGA:
            # Suppress Go Ahead (Standard for character mode)
            response.extend([IAC, WILL, OPT_SGA])
            self.socket.send(response)
            return

        # Default: Refuse everything else to keep it simple
        if command == DO:
            response.extend([IAC, WONT, option])
        elif command == WILL:
            response.extend([IAC, DONT, option])

        if response:
            self.socket.send(response)

    def _handle_subnegotiation(self, data):
        # We don't need to process complex sub-requests for now
        pass

    def close(self):
        """Explicitly shut down the connection."""
        if self.connected:
            self.connected = False
            self.socket.close()
            self.env.kvm.write("\nDisconnected.\n")

def main(env, args):
    client = TelnetClient(env, args)
    client.run()
