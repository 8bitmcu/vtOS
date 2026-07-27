#
# USB Mass Storage sharing for the SD card
# Copyright (c) 2026 8bitmcu
# License: MIT
#
# Blocking: lets a PC mount /sd directly over the same USB-C cable used for
# flashing (composite USB device: existing CDC console + a Mass Storage
# interface). MicroPython and the PC must never write the FAT filesystem at
# the same time, so '/sd' stays unmounted from the device's own VFS -- and
# nothing else can run in this shell -- for as long as this command is
# active. Press any key to stop sharing and get /sd back on-device.
#
import os
import sys

import usb.device
from usb.device.msc import MSCInterface


def main(env, args):
    if env.sd is None:
        print("usbmsc: no SD card detected at boot")
        return

    try:
        os.umount("/sd")
    except OSError as e:
        print(f"usbmsc: failed to unmount /sd: {e}")
        return

    msc = MSCInterface(env.sd)
    try:
        usb.device.get().init(msc, builtin_driver=True)
    except Exception as e:
        print(f"usbmsc: failed to start USB Mass Storage: {e}")
        _remount(env)
        return

    print("usbmsc: /sd is now shared over USB.")
    print("Eject the drive on your PC, then press any key here to stop sharing.")

    try:
        sys.stdin.read(1)
    finally:
        # Always run the cleanup steps below, even if the wait above ends
        # via KeyboardInterrupt (trackball long-click) or anything else --
        # leaving /sd unmounted from the device with no way back is worse
        # than a slightly abrupt USB disconnect.
        usb.device.get().init(builtin_driver=True)  # drop back to CDC console only
        _remount(env)


def _remount(env):
    try:
        os.mount(os.VfsFat(env.sd), "/sd")
        print("usbmsc: /sd remounted locally")
    except OSError as e:
        print(f"usbmsc: failed to remount /sd: {e}")
