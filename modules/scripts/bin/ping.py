#
# MicroPython Ping Network Utility
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import socket
import ustruct as struct
import time

def checksum(data):
    """
    Calculates the standard RFC 1071 internet checksum.
    We iterate through the bytes in pairs (16-bit words), add them up,
    and fold any overflow back into the lower 16 bits.
    """
    if len(data) % 2 != 0:
        data += b'\x00'  # Pad with a zero byte if odd length

    chksum = 0
    for i in range(0, len(data), 2):
        # Combine two bytes into a 16-bit integer (Big Endian)
        word = (data[i] << 8) + data[i + 1]
        chksum += word

    # Fold 32-bit sum into 16 bits
    while (chksum >> 16) > 0:
        chksum = (chksum & 0xFFFF) + (chksum >> 16)

    # One's complement
    chksum = ~chksum & 0xFFFF
    return chksum

def ping(host, count=4, timeout=2.0):
    """
    Sends raw ICMP Echo Requests and listens for the Echo Replies.
    """
    print(f"Pinging {host} with raw sockets...")

    # Resolve the hostname to an IP address
    try:
        dest_addr = socket.getaddrinfo(host, 1)[0][-1][0]
    except OSError:
        print("DNS lookup failed.")
        return

    # Create a raw socket for the ICMP protocol (Protocol number 1)
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_RAW, 1)
    except OSError as e:
        print("Failed to create raw socket. Check your firmware permissions.")
        return

    # Set the socket timeout (in seconds)
    s.settimeout(timeout)

    # Generate an arbitrary packet ID (e.g., 1234)
    packet_id = 1234

    # We will send a 32-byte payload of arbitrary data
    payload = b'abcdefghijklmnopqrstuvwabcdefghi'

    for seq in range(1, count + 1):
        # 1. Build a dummy header with a checksum of 0
        # Format: '!bbHHH' -> Network byte order (Big Endian), 
        # 2 bytes (Type, Code), 3 Unsigned Shorts (Checksum, ID, Sequence)
        # Type 8 = Echo Request, Code 0
        dummy_header = struct.pack('!bbHHH', 8, 0, 0, packet_id, seq)

        # 2. Calculate the real checksum of the dummy header + payload
        packet_chk = checksum(dummy_header + payload)

        # 3. Rebuild the header with the actual checksum
        header = struct.pack('!bbHHH', 8, 0, packet_chk, packet_id, seq)
        packet = header + payload

        # Record the exact microsecond we send the packet
        send_time = time.ticks_us()

        try:
            # Send the packet
            s.sendto(packet, (dest_addr, 1))

            # Wait for the response (64 bytes is enough to catch the IP + ICMP headers)
            resp, addr = s.recvfrom(64)

            # Calculate the round-trip time in milliseconds
            recv_time = time.ticks_us()
            rtt_ms = time.ticks_diff(recv_time, send_time) / 1000.0

            # The raw socket returns the ENTIRE IPv4 packet. 
            # The IPv4 header length is stored in the lower 4 bits of the first byte.
            ip_header_length = (resp[0] & 0x0f) * 4

            # Extract the ICMP header which lives right after the IP header
            icmp_header = resp[ip_header_length:ip_header_length + 8]

            # Unpack the ICMP header to check its Type and Code
            icmp_type, icmp_code, icmp_chk, icmp_id, icmp_seq = struct.unpack('!bbHHH', icmp_header)

            # Type 0 is an Echo Reply. Verify the ID matches our request.
            if icmp_type == 0 and icmp_id == packet_id:
                # Add 20 bytes for the IP header when reporting total bytes received
                print(f"{len(resp)} bytes from {addr[0]}: icmp_seq={icmp_seq} time={rtt_ms:.2f} ms")
            else:
                print(f"Received non-reply ICMP packet: Type {icmp_type}")

        except OSError as e:
            # Timeout or network error
            if e.args[0] == 110: # ETIMEDOUT
                print("Request timeout.")
            else:
                print(f"Socket error: {e}")

        # Sleep for 1 second before the next ping
        time.sleep(1)

    s.close()
    print("Ping complete.")

def main(env, args):
    host = args[0] if len(args) > 0 else None
    count = int(args[1]) if len(args) > 1 else 4

    if host is None:
        print("Usage: ping <host> [count]")
        return

    ping(host, count)

