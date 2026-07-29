#
# MicroPython TUI Internet Radio Player
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import sys
import time

import requests

CONNECT_TIMEOUT = 8       # seconds -- generous: real DNS+TCP connect can take a couple seconds
STREAM_READ_TIMEOUT = 0.2 # seconds -- short steady-state read timeout, armed after connecting

def main(env, args):
    """ Creates a TUI for playing an HTTP MP3 stream (internet radio) """

    url = args[0] if len(args) > 0 else None
    if not url:
        print("Usage: stream <url>")
        return

    vol = env.audio.volume()

    tui = env.tui
    ui_state = "CONNECTING"
    tui.enter_altscreen()
    tui.cursor_hide()

    CLR  = "\x1b[0m"
    BOLD = "\x1b[1m"
    CYAN = "\x1b[38;5;45m"

    resp = None
    err_msg = None

    win = tui.make_window(0, 0,
                          width=env.cols, height=env.rows,
                          title="STREAM",
                          fg=252, bg=18)

    # Draw "Connecting..." before the blocking requests.get() call below --
    # DNS + TCP connect + header parse can visibly take a couple of seconds
    # on real WiFi, and requests.get() runs synchronously on the main
    # thread with nothing else able to run meanwhile.
    win.invalidate()
    win.draw()
    win.draw_label("Connecting to:",
                  0, win.inner_h // 2,
                  fg=252, bg=18,
                  align="center")
    win.draw_label(url,
                  0, (win.inner_h // 2)+1,
                  fg=252, bg=18,
                  align="center")
    tui.draw()

    try:
        resp = requests.get(url, headers={"Icy-MetaData": "0"}, timeout=CONNECT_TIMEOUT)
        if resp.status_code != 200:
            err_msg = f"HTTP {resp.status_code} {resp.reason}"
        else:
            # Re-arm a short timeout for steady-state body reads, separate
            # from the generous connect-phase timeout above. If the server
            # unexpectedly sent Content-Length, resp.raw is a
            # requests.BodyStream wrapper rather than the bare socket --
            # it has no settimeout() of its own, so guard with hasattr().
            if hasattr(resp.raw, "settimeout"):
                resp.raw.settimeout(STREAM_READ_TIMEOUT)

            # Hand the live socket straight to the native player -- it
            # already implements the stream protocol (read/close), and
            # audioplayer.c's play() accepts any stream-like object
            # directly (not just a path). The C module owns this socket's
            # lifecycle from here on (closed via stop()/EOF/hard-error);
            # deliberately do NOT call resp.close() ourselves.
            env.audio.play(resp.raw)
            ui_state = "PLAYING"
    except OSError as e:
        err_msg = f"Connection failed: {e}"
    except Exception as e:
        err_msg = str(e)

    if err_msg:
        ui_state = "ERROR"

    if ui_state == "PLAYING":
        time.sleep_ms(1000)

    while True:
        win = tui.make_window(
                0, 0,
                width=env.cols, height=env.rows,
                title="STREAM",
                fg=252, bg=18)

        if ui_state == "PLAYING":

            blk = win.make_block(f"{BOLD}Streaming:{CLR}\n{CYAN}{url}{CLR}\n"
                                 f"\n"
                                 f"Volume: {vol}%",
                                 0, 0,
                                 fg=252, bg=18,
                                 wrap=True)

            status = win.make_label("[w/s] vol | [p]ause/resume | [q]uit",
                                    0, win.inner_h-1,
                                    fg=0, bg=252,
                                    width=win.inner_w)

            win.invalidate()
            while True:
                win.draw()
                blk.draw()
                status.draw()
                tui.draw()

                char = sys.stdin.read(1)
                if char == "w":
                    if vol + 10 <= 100:
                        vol = vol + 10
                        env.audio.volume(vol)
                        env.volume = vol
                        break
                elif char == "s":
                    if vol - 10 >= 0:
                        vol = vol - 10
                        env.audio.volume(vol)
                        env.volume = vol
                        break
                elif char == "p":
                    if env.audio.is_paused():
                        env.audio.resume()
                    else:
                        env.audio.pause()
                    break
                elif char == "q":
                    ui_state = "QUIT"
                    break

        elif ui_state == "ERROR":
            blk = win.make_block(f"{BOLD}Error:{CLR}\n{err_msg}",
                           0, 0,
                           fg=252, bg=18,
                           wrap=True)

            status = win.make_label("[any key] quit",
                                    0, win.inner_h-1,
                                    fg=0, bg=252,
                                    width=win.inner_w)

            win.invalidate()
            win.draw()
            blk.draw()
            status.draw()
            tui.draw()

            sys.stdin.read(1)
            ui_state = "QUIT"

        elif ui_state == "QUIT":
            tui.exit_altscreen()
            tui.cursor_show()

            if env.audio.is_playing():
                env.audio.stop()
                err = env.audio.last_error()
                if err != 0:
                    print("playback ended with error code", err)

            env.audio.deinit()
            env.audio = None

            return
