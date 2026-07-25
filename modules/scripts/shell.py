#
# MicroPython Shell
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import sys
import json
import board

def _app(module, tui=False, audio=False, rec=False, radio=False):
    def _run(env, *args):
        if tui:
            if not hasattr(env, 'tui') or env.tui is None:
                import vttui
                env.tui = vttui.VTTUI(env, env.cols, env.rows)
        if audio:
            if not hasattr(env, 'audio') or env.audio is None:
                import audioplayer
                env.audio = audioplayer.AudioPlayer(bck=board.I2S_BCK,
                                                    ws=board.I2S_WS,
                                                    dout=board.I2S_DOUT)
        if rec:
            if not hasattr(env, 'rec') or env.rec is None:
                import audioplayer
                env.rec = audioplayer.AudioRecorder(mclk=board.ES7210_MCLK,
                                                    bck=board.ES7210_SCK,
                                                    ws=board.ES7210_LRCK,
                                                    din=board.ES7210_DIN,
                                                    i2c_sda=board.I2C_SDA,
                                                    i2c_scl=board.I2C_SCL,
                                                    i2s_num=1,
                                                    channels=1,
                                                    mic_gain=9,
                                                    dma_buf_count=6,
                                                    sample_rate=8000,
                                                    mclk_ratio=512,
                                                    i2c_shared=True)

        if radio:
            if not hasattr(env, 'radio') or env.radio is None:
                import lora
                lr = lora.LoRa(cs=board.RADIO_CS,
                               dio1=board.RADIO_DIO1,
                               rst=board.RADIO_RST,
                               busy=board.RADIO_BUSY,
                               sck=board.SPI_SCK,
                               miso=board.SPI_MISO,
                               mosi=board.SPI_MOSI)

                try:
                    with open("/flash/.radio.json", "r") as f:
                        config = json.load(f)
                except Exception as e:
                    print("Failed to load radio config. Have you run loracfg?")
                    return

                try:
                    lr.begin(freq=float(config["freq"]),
                             bw=board.RADIO_BANDWIDTH,
                             sf=board.RADIO_SF,
                             cr=board.RADIO_CR,
                             sync_word=0x12,
                             power=int(config["pwr"]))
                except Exception as e:
                    print(f"Radio initialization failed: {e}")
                    return

                errors = lr.get_device_errors()
                if errors != 0x0000:
                    print(f"SX1262 device errors: 0x{errors:04X}")
                    return

                env.radio = lr


        app_module = __import__(module, None, None, [''])
        app_module.main(env, args)
    return _run

class Command:
    def __init__(self, func, env):
        self.func = func
        self.env = env

    def execute(self, *args):
        try:
            self.func(self.env, *args)
            return True
        except Exception as e:
            print("\r\n[!] App Exception Caught:")
            sys.print_exception(e)
            return False

class Shell:
    _MAX_HISTORY = 20

    def __init__(self, env):
        self.env = env
        self.apps = {}
        self.running = True
        self._history = []

        self.aliases = {}
        self.alias_file = "/flash/.favs.json"
        self._load_aliases()

        self.register("ping",        _app("applications.ping"))
        self.register("sshd",        _app("applications.sshd"))
        self.register("ssh",         _app("applications.ssh"))
        self.register("sftpd",       _app("applications.sftpd"))
        self.register("webvncd",     _app("applications.webvncd"))
        self.register("vncd",        _app("applications.vncd"))
        self.register("chess",       _app("applications.chess"))
        self.register("c2",          _app("applications.c2"))
        self.register("ftp",         _app("applications.ftp"))
        self.register("ftpd",        _app("applications.ftpd"))
        self.register("sftp",        _app("applications.sftp"))
        self.register("telnet",      _app("applications.telnet"))
        self.register("ms",          _app("applications.minesweeper"))
        self.register("loracfg",     _app("applications.loracfg"))
        self.register("menu",        _app("applications.menu",       tui=True))
        self.register("nm",          _app("applications.netmgr",     tui=True))
        self.register("fm",          _app("applications.filemgr",    tui=True))
        self.register("irc",         _app("applications.irc",        tui=True))
        self.register("rss",         _app("applications.rss",        tui=True))
        self.register("fc",          _app("applications.fontcfg",    tui=True))
        self.register("play",        _app("applications.player",     tui=True, audio=True))
        self.register("lorachat",    _app("applications.lorachat",   tui=True, radio=True))
        self.register("rec",         _app("applications.rec",        rec=True))
        self.register("vi",          _app("vimod"))
        self.register("zm",          _app("zm"))

    def _load_aliases(self):
        try:
            with open(self.alias_file, "r") as f:
                self.aliases = json.load(f)
        except (OSError, ValueError):
            pass

    def _save_aliases(self):
        try:
            with open(self.alias_file, "w") as f:
                json.dump(self.aliases, f)
        except OSError as e:
            print(f"Failed to save favs: {e}")

    def register(self, name, func):
        self.apps[name] = Command(func, self.env)

    def parse_args(self, line):
        """Split a command line respecting single and double quoted strings."""
        parts = []
        current = []
        in_quote = None
        for ch in line:
            if in_quote:
                if ch == in_quote:
                    in_quote = None
                else:
                    current.append(ch)
            elif ch in ('"', "'"):
                in_quote = ch
            elif ch == ' ':
                if current:
                    parts.append(''.join(current))
                    current = []
            else:
                current.append(ch)
        if current:
            parts.append(''.join(current))
        return parts

    def execute(self, cmd_name, *args):
        if cmd_name in self.apps:
            return self.apps[cmd_name].execute(*args)
        else:
            print(f"{cmd_name}: command not found")
            return False

    def _read_line(self, prompt):
        print(prompt, end="")
        buffer = ""
        hist_idx = len(self._history)  # one past end = live input
        saved = ""                     # stash for in-progress line while browsing

        while True:
            char = sys.stdin.read(1)
            if not char:
                continue

            if char == '\x01':
                # Ctrl-A: mpremote raw REPL request. Re-inject the byte so
                # the MicroPython C REPL loop sees it after the shell exits,
                # then exit so it can handle the raw REPL handshake.
                self.env.kvm.inject('\x01')
                raise EOFError

            if char == '\x1b':
                # Consume ESC [ A/B sequences (3 bytes total)
                nxt = sys.stdin.read(1)
                if nxt == '[':
                    seq = sys.stdin.read(1)
                    if seq == 'A' and self._history:        # UP
                        if hist_idx == len(self._history):
                            saved = buffer
                        if hist_idx > 0:
                            hist_idx -= 1
                            new_buf = self._history[hist_idx]
                            print('\b \b' * len(buffer) + new_buf, end='')
                            buffer = new_buf
                    elif seq == 'B' and self._history:      # DOWN
                        if hist_idx < len(self._history):
                            hist_idx += 1
                            new_buf = self._history[hist_idx] if hist_idx < len(self._history) else saved
                            print('\b \b' * len(buffer) + new_buf, end='')
                            buffer = new_buf
                else:
                    # Bare ESC (trackball click): clear the current line
                    print('\b \b' * len(buffer), end='')
                    buffer = ""
                    hist_idx = len(self._history)
                    saved = ""
                continue

            if char in ('\r', '\n'):
                print("\r")
                return buffer.strip()

            elif char in ('\x08', '\x7f'):
                if buffer:
                    buffer = buffer[:-1]
                    print('\b \b', end='')

            elif 32 <= ord(char) <= 126:
                buffer += char
                print(char, end='')

    def run(self):
        ver = sys.implementation.version
        version_str = f"{ver[0]}.{ver[1]}.{ver[2]}"
        # TODO: move versioning to makefile
        print(f"vtOS v0.1.11; MicroPython v{version_str}\nType 'help' to see commands.")

        while self.running:
            try:
                user_input = self._read_line("\033[38;5;85m$\033[0m ")
            except EOFError:
                break  # mpremote Ctrl-A: exit cleanly, C REPL takes over
            except KeyboardInterrupt:
                print("\r\nType 'exit' to quit.")
                continue

            if not user_input:
                continue

            parts = self.parse_args(user_input)
            cmd_name = parts[0]
            args = parts[1:]

            if cmd_name in self.aliases:
                expanded_parts = self.parse_args(self.aliases[cmd_name])
                if expanded_parts:
                    cmd_name = expanded_parts[0]
                    args = expanded_parts[1:] + args

            if cmd_name == "fav":
                if not args:
                    # List all aliases
                    if not self.aliases:
                        print("No favs set. Use: fav <name> <command>")
                    for k, v in sorted(self.aliases.items()):
                        print(f"  {k} -> {v}")

                elif args[0] == "rm" and len(args) == 2:
                    # Remove an alias (e.g., fav rm myftp)
                    key = args[1]
                    if key in self.aliases:
                        del self.aliases[key]
                        self._save_aliases()
                        print(f"Removed fav '{key}'.")
                    else:
                        print(f"fav '{key}' not found.")

                elif len(args) >= 2:
                    # Create or update an alias
                    key = args[0]

                    # Reconstruct the target command, restoring quotes if spaces exist
                    val_parts = []
                    for a in args[1:]:
                        val_parts.append(f'"{a}"' if ' ' in a else a)

                    val = " ".join(val_parts)
                    self.aliases[key] = val
                    self._save_aliases()
                    print(f"Saved fav: {key} -> {val}")
                continue

            if cmd_name == "clear":
                print("\033[2J\033[H", end="")
                continue

            elif cmd_name == "dbgrst":
                import machine
                import vt
                _reset_names = {
                    machine.PWRON_RESET: "PWRON_RESET (power-on)",
                    machine.HARD_RESET: "HARD_RESET (panic / external reset)",
                    machine.WDT_RESET: "WDT_RESET (watchdog timeout)",
                    machine.DEEPSLEEP_RESET: "DEEPSLEEP_RESET (woke from deep sleep)",
                    machine.SOFT_RESET: "SOFT_RESET (soft reboot)",
                }
                _reset_cause = machine.reset_cause()
                print("Last reset cause: %s [%d]" % (_reset_names.get(_reset_cause, "UNKNOWN"), _reset_cause))

                # machine.reset_cause() buckets several distinct ESP-IDF
                # reset reasons (e.g. INT_WDT/TASK_WDT/the RTC-level WDT)
                # into one WDT_RESET -- vt.reset_reason() surfaces the raw
                # esp_reset_reason_t value so they can be told apart.
                _esp_reset_names = {
                    0: "ESP_RST_UNKNOWN",
                    1: "ESP_RST_POWERON",
                    2: "ESP_RST_EXT",
                    3: "ESP_RST_SW",
                    4: "ESP_RST_PANIC",
                    5: "ESP_RST_INT_WDT",
                    6: "ESP_RST_TASK_WDT",
                    7: "ESP_RST_WDT (RTC-level watchdog)",
                    8: "ESP_RST_DEEPSLEEP",
                    9: "ESP_RST_BROWNOUT",
                    10: "ESP_RST_SDIO",
                }
                _esp_reset_cause = vt.reset_reason()
                print("ESP reset reason: %s [%d]" % (
                    _esp_reset_names.get(_esp_reset_cause, "UNKNOWN"), _esp_reset_cause))
                continue

            elif cmd_name == "exit":
                break

            elif cmd_name == "help":
                sorted_apps = sorted(self.apps.keys())
                print("Available commands:", ", ".join(sorted_apps))
                continue

            # Record in history, skipping consecutive duplicates
            if not self._history or self._history[-1] != user_input:
                if len(self._history) >= self._MAX_HISTORY:
                    self._history.pop(0)
                self._history.append(user_input)

            self.execute(cmd_name, *args)

