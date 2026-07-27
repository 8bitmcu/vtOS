#
# MicroPython TUI LoRa Chat
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

def _format_incoming(nick, text, rssi):
    return f"\x1b[1m{nick}\x1b[22m: {text}  \x1b[38;5;244m({rssi:.0f} dBm)\x1b[0m"

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

    chat_win.push(f"--- LoRaChat ready as {nick} ---\n/nick <name>\n/quit or ESC to leave\n")
    chat_win.draw()
    env.tui.draw()

    env.tui.cursor_show()
    env.radio.start_receive()

    while True:
        chat_win.draw()
        chat_input.draw()
        env.tui.draw()

        rlist, _, _ = select.select([sys.stdin], [], [], 0.1)

        # LoRa has no file descriptor to select() on, so poll it directly
        # every loop iteration regardless of what stdin is doing.
        packet = env.radio.receive()
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

            chat_win.push(CYAN + _format_incoming(peer_nick, peer_text, env.radio.rssi()) + CLR + "\n")

            # The chip drops back to standby after a read; re-arm to keep listening.
            env.radio.start_receive()

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
                    env.tui.exit_altscreen()
                    env.tui.cursor_show()
                    return
                else:
                    msg = f"{nick}: {text}"
                    try:
                        env.radio.transmit(msg.encode('utf-8'))
                        chat_win.push(RED + f"\x1b[1m{nick}\x1b[22m: {text}" + CLR + "\n")
                    except RuntimeError as e:
                        chat_win.push(f"--- Send failed: {e} ---\n")
                    finally:
                        # transmit() puts the chip in TX then back to
                        # standby (win or lose) -- re-arm RX so we're
                        # listening again for replies.
                        env.radio.start_receive()
                    chat_input.set("")

            elif char in ('\x08', '\x7f'):
                chat_input.backspace()
            elif char == '\x1b':
                env.tui.exit_altscreen()
                env.tui.cursor_show()
                return
            else:
                chat_input.push(char)
