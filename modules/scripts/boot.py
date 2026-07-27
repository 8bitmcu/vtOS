import os
import sys
import machine
import micropython

import env
import hardware
import vt
import tdeck_kvm
import tdeck_trk
import statusbar
import shell
import defaults


env = env.Environment(320, 240, "terminus_mpy_14", "siji_mpy_statusbar_12")

hardware.init_flash()
defaults.apply()
hardware.init_board()
hardware.init_spi(env)
tft = hardware.init_tft(env)
kbd = hardware.init_keyboard()

# Initialize ST engine
env.term = vt.VT(tft, env)
env.term.top_offset(env.status_height)
env.term.set_icon_font(env.icon_font)


# Combine ST & keyboard into one stream object
env.kvm = tdeck_kvm.KVM(env.term, kbd)

# Redirect to REPL
os.dupterm(env.kvm)

# Status bar component
env.sts = statusbar.StatusBar(env.term, env, width=env.cols)
env.sts.refresh()

# The FAST loop (30ms)
def scheduled_fast(_):

    # Trackball horizontal movement translates into going up/down the shell command history
    h_delta = tdeck_trk.get_scroll_horiz()
    if abs(h_delta) > 1:
        if h_delta < 0:
            env.kvm.inject("\x1b[A") # Injects 'Up' key into REPL
        else:
            env.kvm.inject("\x1b[B") # Injects 'Down' key into REPL

    # Trackball vertical movement translates into showing history
    # Default history is 100 lines defined as HISTSIZE in st.h
    v_delta = tdeck_trk.get_scroll_vert()
    if abs(v_delta) > 1:
        if v_delta < 0:
            env.term.scrolldown()
        else:
            env.term.scrollup()

    # Long clicking will raise KeyboardInterrupt (internally to tdeck_trk)
    # Short click will inject escape
    if tdeck_trk.get_click():
        env.kvm.inject("\x1b")

    # Skip this tick's redraw rather than racing an in-flight SD transfer
    # that has the shared SPI bus temporarily parked at SD_BAUDRATE.
    if not env.sd_busy:
        env.term.draw()

def fast_loop(_):
    # Use schedule to keep the ISR (Interrupt Service Routine) light.
    # schedule() raises RuntimeError if its queue is already full (e.g.
    # mp_task is blocked in some other call and hasn't drained the
    # previous tick yet) -- letting that escape uncaught from a hard-IRQ
    # timer callback is unsafe, so just drop this tick instead.
    try:
        micropython.schedule(scheduled_fast, 0)
    except RuntimeError:
        pass

# The SLOW loop (1000ms)
def scheduled_slow(_):
    # refresh() itself checks env.sd_busy before touching the display --
    # see statusbar.py.
    env.sts.refresh()

def slow_loop(_):
    # Update the status bar string (ANSI parsing happens here)
    try:
        micropython.schedule(scheduled_slow, 0)
    except RuntimeError:
        pass

# 30ms = ~33 FPS.
draw_timer = machine.Timer(0)
draw_timer.init(period=30, mode=machine.Timer.PERIODIC, callback=fast_loop)

statusbar_timer = machine.Timer(1)
statusbar_timer.init(period=1000, mode=machine.Timer.PERIODIC, callback=slow_loop)

# Choose your cursor:

# Beam Cursor
sys.stdout.write("\x1b[ 6 q")

# Underline Cursor
#sys.stdout.write("\x1b[ 4 q")

# Block Cursor (default)
#sys.stdout.write("\x1b[ 2 q")

class Cmd:
    def __init__(self, func):
        self.func = func
    def __repr__(self):
        self.func()
        return ""

env.shell = shell.Shell(env)

# quick way to return to the shell from MicroPython
# just type `sh` into MicroPython to return to our shell
sh = Cmd(env.shell.run)

# "REPL" into a custom, simple shell
env.shell.run()

