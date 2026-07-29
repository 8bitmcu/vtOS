#
# MicroPython TUI BLE Mesh Chat
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import select
import sys
import random

def _default_nick():
    """Generates a random nickname in the format vtOS_XXXX."""
    chars = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789'
    rand = ''.join(random.choice(chars) for _ in range(4))
    return f"vtOS_{rand}"

def _leave(env):
    """Releases the BLE radio's memory back to the heap on the way out --
    without this, it stays reserved for the rest of the boot session even
    after you're done chatting (see BLEMesh.deinit's docstring)."""
    env.ble.deinit()
    env.ble = None
    env.tui.exit_altscreen()
    env.tui.cursor_show()

def _format_incoming(nick, text, rssi):
    if rssi is None:
        return f"\x1b[1m{nick}\x1b[22m: {text}"
    return f"\x1b[1m{nick}\x1b[22m: {text}  \x1b[38;5;244m({rssi} dBm)\x1b[0m"

def main(env, args):
    nick = args[0] if len(args) > 0 else _default_nick()

    CLR  = "\x1b[0m"
    RED  = "\x1b[38;5;210m"
    CYAN = "\x1b[38;5;45m"

    env.tui.enter_altscreen()
    env.tui.cursor_hide()

    chat_win = env.tui.make_block("", 0, 0,
          width=env.cols, height=env.rows-1,
          fg=252, bg=18,
          scroll=True, wrap=True)

    chat_input = env.tui.make_input("",
        0, env.rows-1,
        width=env.cols,
        fg=252, bg=0, input_bg=0,
        decorations=False)

    chat_win.push(f"--- BLEChat ready as {nick} ---\n/nick <name>\n/quit or ESC to leave\n")
    chat_win.draw()
    env.tui.draw()

    env.tui.cursor_show()

    # Unlike LoRa's half-duplex antenna, BLE scanning runs continuously
    # in the background from the moment env.ble was constructed -- there's
    # no start_receive()/re-arm step needed here.

    while True:
        chat_win.draw()
        chat_input.draw()
        env.tui.draw()

        rlist, _, _ = select.select([sys.stdin], [], [], 0.1)

        # BLE has no file descriptor to select() on, so poll it directly
        # every loop iteration regardless of what stdin is doing.
        packet = env.ble.receive()
        if packet:
            try:
                text = packet.decode('utf-8')
            except UnicodeError:
                text = repr(packet)

            # Messages are sent as "nick: text" (see the send path below);
            # split that back out for display, but don't choke on a peer
            # that isn't following the convention.
            if ': ' in text:
                peer_nick, _, peer_text = text.partition(': ')
            else:
                peer_nick, peer_text = '?', text

            chat_win.push(CYAN + _format_incoming(peer_nick, peer_text, env.ble.last_rssi()) + CLR + "\n")

        if sys.stdin in rlist:
            char = sys.stdin.read(1)
            if char in ('\r', '\n'):
                text = chat_input.value
                if not text:
                    continue

                if text.startswith("/nick "):
                    nick = text.split(" ", 1)[1]
                    chat_win.push(f"--- Nick changed to {nick} ---\n")
                    chat_input.set("")
                elif text == "/quit":
                    _leave(env)
                    return
                else:
                    msg = f"{nick}: {text}"
                    try:
                        env.ble.broadcast(msg.encode('utf-8'))
                        chat_win.push(RED + f"\x1b[1m{nick}\x1b[22m: {text}" + CLR + "\n")
                    except ValueError as e:
                        chat_win.push(f"--- Send failed: {e} ---\n")
                    chat_input.set("")

            elif char in ('\x08', '\x7f'):
                chat_input.backspace()
            elif char == '\x1b':
                _leave(env)
                return
            else:
                chat_input.push(char)
