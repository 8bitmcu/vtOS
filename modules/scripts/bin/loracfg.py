#
# MicroPython LoRa Configuration Utility
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import json

def main(env, args):

    if not args:
        print("Usage: loracfg <frequency> [power]")
        return

    frequency = float(args[0]) if len(args) > 0 else 0

    if frequency == 0:
        print("Invalid frequency")
        return

    power = int(args[1]) if len(args) > 1 else 10

    with open("/flash/.radio.json", "w") as f:
        json.dump({
            "freq": frequency,
            "pwr": power
        }, f)

