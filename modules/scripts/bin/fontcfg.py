#
# MicroPython CLI Font Configuration Utility
# Copyright (c) 2026 8bitmcu
# License: MIT
#

def main(env, args):
    """ CLI for changing system font """

    if not args:
        print("Usage: fc <fontname>")
        return

    env.update_font(args[0])
    print("Font updated to " + args[0])
