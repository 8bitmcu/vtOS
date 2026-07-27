#
# MicroPython Audio Recording utility
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import os
import sys
import time
import bin.c2

def fmt_size(size):
    if size < 1024:
        return f"{size}B"
    elif size < 1024 * 1024:
        return f"{size / 1024:.1f}K"
    elif size < 1024 * 1024 * 1024:
        return f"{size / (1024 * 1024):.1f}M"
    else:
        return f"{size / (1024 * 1024 * 1024):.1f}G"

def main(env, args):

    if not args:
        print("Usage: rec <file> [length]")
        return

    file_name = args[0]
    length = int(args[1]) if len(args) > 1 else 0

    to_c2 = file_name.endswith(".c2")
    record_path = file_name + ".wav" if to_c2 else file_name   # always record to WAV first

    print("Recording...")
    if length > 0:
        env.rec.record(record_path, seconds=length)
        while env.rec.is_recording():
            time.sleep_ms(200)
    else:
        print("Press any key to stop recording.")
        env.rec.record(record_path)
        sys.stdin.read(1)

    bytes_written = env.rec.stop()
    print(f"Recorded {fmt_size(bytes_written)}!")

    if to_c2:
        print(f"Encoding to {file_name}...")
        wav_size = os.stat(record_path)[6]
        frames = bin.c2.encode(record_path, file_name, bin.c2.DEFAULT_ENCODE_MODE)
        os.remove(record_path)
        c2_size = os.stat(file_name)[6]
        ratio_pct = (c2_size / wav_size * 100) if wav_size else 0
        print(f"Encoded {frames} frame(s), {fmt_size(c2_size)}\n"
              f"({ratio_pct:.2f}% of original {fmt_size(wav_size)})")
