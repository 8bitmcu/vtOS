#
# Quick on-device previewer for iconic fonts
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import sys

def main(env, args):
    font_name = args[0] if len(args) > 0 else "siji_mpy_12"

    old_font = env.icon_font_name
    env.update_icon_font(font_name)
    font = env.icon_font

    if hasattr(font, "WIDE_CHARS"):
        codepoints = list(font.WIDE_CHARS)
    else:
        codepoints = list(range(font.WIDE_FIRST, font.WIDE_FIRST + font.WIDE_COUNT))

    start = 0
    total = len(codepoints)
    count = int(args[2]) if len(args) > 2 else total
    end = min(start + count, total)
    print(f"{font_name}: {total} icons\nshowing {start} to {end - 1}")

    for i in range(start, end):
        codepoint = codepoints[i]
        ch = chr(codepoint)
        if (i - start) % 16 == 0:
            sys.stdout.write(f"\nU+{codepoint:04X} ")
        sys.stdout.write(f"{ch}")
    sys.stdout.write("\n")

    print("Press any key to exit.")
    sys.stdin.read(1)

    env.update_icon_font(old_font)

