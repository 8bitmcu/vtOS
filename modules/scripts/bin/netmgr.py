#
# MicroPython CLI/TUI Network Manager
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import sys
import time
import network

def scan_for_networks():
    """Initializes the Wi-Fi interface and gathers clean metadata targets."""

    wlan = network.WLAN(network.STA_IF)
    wlan.active(True)
    raw_scan = wlan.scan()

    sorted_scan = sorted(raw_scan, key=lambda x: x[3], reverse=True)

    menu_data = []
    for ap in sorted_scan:
        ssid = ap[0].decode('utf-8', 'ignore').strip()
        rssi = ap[3]
        authmode = ap[4]

        if ssid:
            lock = "[P] " if authmode > 0 else "    "
            display_str = f"{lock}{ssid} ({rssi}dBm)"
            menu_data.append((display_str, ssid, authmode))

    return menu_data

def main(env, args):
    """ Creates a CLI/TUI for connecting or creating a Wi-Fi network"""
    action = args[0] if len(args) > 0 else None
    ssid = args[1] if len(args) > 1 else None
    pwd = args[2] if len(args) > 2 else None

    if args and (action != "connect" or len(args) < 2):
        print("Usage: nm connect <ssid> [password]")
        return

    if args and action == "connect":
        wlan = network.WLAN(network.STA_IF)
        wlan.active(True)

        if wlan.isconnected():
            wlan.disconnect()
            time.sleep(0.5)

        if pwd:
            wlan.connect(ssid, pwd)
        else:
            wlan.connect(ssid)

        timeout = 0
        print("Connecting", end="")
        while not wlan.isconnected():
            sys.stdout.write(".")
            time.sleep(1)
            timeout += 1
            if timeout >= 15:
                break

        if wlan.isconnected():
            print(f"\nConnected! IP: {wlan.ifconfig()[0]}")
        else:
            print("\nConnection failed (timed out).")

        return


    tui = env.tui
    lst = None
    ui_state = "MAIN_MENU"
    tui.enter_altscreen()
    tui.cursor_hide()
    password = ""

    CLR  = "\x1b[0m"
    BOLD = "\x1b[1m"
    GREEN = "\x1b[38;5;121m"
    RED = "\x1b[38;5;210m"

    win = tui.make_window(
            0, 0,
            width=env.cols, height=env.rows,
            title="NETWORK MANAGER",
            fg=252, bg=18)

    while True:
        if ui_state == "MAIN_MENU":
            lst = None
            password = ""

            wlan = network.WLAN(network.STA_IF)
            wlan.active(True)
            if wlan.isconnected():
                ip, subnet, gateway, dns = wlan.ifconfig()

                blk = win.make_block(
                    f"{BOLD}Network Status :{CLR}  {GREEN}Connected!{CLR}\n"
                    f"\n"
                    f"{BOLD}Network Configuration: {CLR}\n"
                    f"  {BOLD}IP Address :{CLR}  {GREEN}{ip}{CLR}\n"
                    f"  {BOLD}Subnet     :{CLR}  {subnet}\n"
                    f"  {BOLD}Gateway    :{CLR}  {gateway}\n"
                    f"  {BOLD}DNS Server :{CLR}  {dns}",
                    2, 2,
                    fg=252, bg=18)

                status = win.make_label(
                    "[d]isconnect | [q]uit",
                    0, win.inner_h - 1,
                    fg=0, bg=252,
                    width=win.inner_w)

                win.invalidate()
                win.draw()
                blk.draw()
                status.draw()
                tui.draw()

                while True:
                    char = sys.stdin.read(1)
                    if char == "d":
                        wlan.disconnect()
                        break
                    elif char == "q":
                        ui_state  = "QUIT"
                        break
            else:
                blk = win.make_block(f"{BOLD}Network Status :{CLR}  {RED}Disconnected{CLR}\n",
                    2, 2,
                    fg=252, bg=18)

                status = win.make_label(
                    "[c]onnect | [n]ew | [q]uit",
                    0, win.inner_h - 1,
                    fg=0, bg=252,
                    width=win.inner_w)

                win.invalidate()
                win.draw()
                blk.draw()
                status.draw()
                tui.draw()

                while True:
                    char = sys.stdin.read(1)
                    if char == "c":
                        ui_state = "SCAN_WIFI"
                        break
                    if char == "n":
                        ui_state = "CREATE_AP"
                        break
                    elif char == "q":
                        ui_state = "QUIT"
                        break

        elif ui_state == "SCAN_WIFI":

            scanning = win.make_label(
                    "Scanning...",
                    0, win.inner_h//2,
                    fg=252, bg=18,
                    align="center")

            win.invalidate()
            win.draw()
            scanning.draw()
            tui.draw()

            networks = scan_for_networks()
            if networks:
                labels = [n[0] for n in networks]
                lst = win.make_list(
                        labels,
                        title="Select a Network:",
                        x=0, y=2,
                        width=env.cols-10, height=win.inner_h-6,
                        fg=252, bg=18,
                        arrow=">", left_pad=1,
                        align="center")
            else:
                no_networks = win.make_label(
                        "No networks found.",
                        0, win.inner_h//2,
                        fg=252, bg=18,
                        align="center")

            status = win.make_label(
                    "[w/s] nav | [r]efrsh | [b]ack | [q]uit",
                    0, win.inner_h-1,
                    fg=0, bg=252,
                    width=win.inner_w)

            while True:
                win.draw()
                if lst:
                    lst.draw()
                else:
                    no_networks.draw()
                status.draw()
                tui.draw()

                char = sys.stdin.read(1)
                if char == "w" and lst:
                    lst.up()
                elif char == "s" and lst:
                    lst.down()
                elif (char == '\r' or char == '\n') and lst:
                    ui_state = "INPUT_PWD" if networks[lst.selected][2] > 0 else "CONNECT_WIFI"
                    break
                elif char == "b":
                    ui_state = "MAIN_MENU"
                    break
                elif char == "r":
                    break
                elif char == "q":
                    ui_state = "QUIT"
                    break

        elif ui_state == "INPUT_PWD" and lst is not None:

            pwd_label = win.make_label(
                    "Password required for network: ",
                    0, 1,
                    fg=252, bg=18,
                    align="center")

            pwd_label2 = win.make_label(
                    networks[lst.selected][1],
                    0, 2,
                    fg=252, bg=18,
                    bold=True,
                    align="center")

            pwd = win.make_input("Password:",
                    0, 4,
                    width=win.inner_w-2,
                    fg=252, bg=18, input_bg=0,
                    secret=True,
                    align="center")

            tui.cursor_show()
            win.invalidate()
            win.draw()
            pwd_label.draw()
            pwd_label2.draw()
            pwd.draw()
            tui.draw()

            while True:
                char = sys.stdin.read(1)
                if char in ('\r', '\n'):
                    password = pwd.value
                    tui.cursor_hide()
                    ui_state = "CONNECT_WIFI"
                    break
                elif char in ('\x08', '\x7f'):  # backspace
                    pwd.backspace()
                elif char == '\x1b':            # escape: back to scan wifi
                    tui.cursor_hide()
                    ui_state = "SCAN_WIFI"
                    break
                else:
                    pwd.push(char)
                pwd.draw()
                tui.draw()

        elif ui_state == "CONNECT_WIFI" and lst is not None:
            wlan = network.WLAN(network.STA_IF)
            wlan.active(True)

            if wlan.isconnected():
                wlan.disconnect()
                time.sleep(0.5)

            if password:
                wlan.connect(networks[lst.selected][1], password)
            else:
                wlan.connect(networks[lst.selected][1])

            win.invalidate()
            win.draw()
            tui.draw()

            timeout = 0
            dots = ["   ", ".  ", ".. ", "..."]
            while not wlan.isconnected():
                win.draw_label(f"Connecting{dots[timeout % 4]}",
                               0, win.inner_h // 2,
                               fg=252, bg=18,
                               align="center")
                tui.draw()
                time.sleep(1)
                timeout += 1
                if timeout >= 15:
                    break

            if not wlan.isconnected():
                win.invalidate()
                win.draw()
                win.draw_label("Connection failed. Press any key.",
                               0, win.inner_h // 2,
                               fg=210, bg=18,
                               align="center")
                tui.draw()
                sys.stdin.read(1)

            ui_state = "MAIN_MENU"

        elif ui_state == "CREATE_AP":
            # TODO; currently not supported
            ui_state = "MAIN_MENU"

        elif ui_state == "QUIT":
            tui.exit_altscreen()
            tui.cursor_show()
            return

