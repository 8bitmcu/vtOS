#
# MicroPython Shell
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import gc
import sys
import json
import hardware

def _app(module, tui=False, audio=False, rec=False, radio=False, ble=False):
    def _run(env, *args):
        if tui:
            if not hasattr(env, 'tui') or env.tui is None:
                import modtui
                env.tui = modtui.VTTUI(env, env.cols, env.rows)

        if audio:
            if not hasattr(env, 'audio') or env.audio is None:
                hardware.init_audio(env)
            if not hasattr(env, 'audio') or env.audio is None:
                return

        if rec:
            if not hasattr(env, 'rec') or env.rec is None:
                hardware.init_mic(env)
            if not hasattr(env, 'rec') or env.rec is None:
                return

        if radio:
            if not hasattr(env, 'radio') or env.radio is None:
                hardware.init_radio(env)
            if not hasattr(env, 'radio') or env.radio is None:
                return

        if ble:
            if not hasattr(env, 'ble') or env.ble is None:
                hardware.init_ble(env)
            if not hasattr(env, 'ble') or env.ble is None:
                return

        app_module = __import__(module, None, None, [''])
        app_module.main(env, args)
        gc.collect()
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
        self._rc_ran = False  # .shellrc runs once per boot -- see run()

        self.aliases = {}
        self.alias_file = "/flash/.favs.json"
        self._load_aliases()

        self.register("ping",        _app("bin.ping"))
        self.register("sshd",        _app("bin.sshd"))
        self.register("ssh",         _app("bin.ssh"))
        self.register("sftpd",       _app("bin.sftpd"))
        self.register("webvncd",     _app("bin.webvncd"))
        self.register("vncd",        _app("bin.vncd"))
        self.register("chess",       _app("bin.chess"))
        self.register("c2",          _app("bin.c2"))
        self.register("ftp",         _app("bin.ftp"))
        self.register("ftpd",        _app("bin.ftpd"))
        self.register("sftp",        _app("bin.sftp"))
        self.register("telnet",      _app("bin.telnet"))
        self.register("telnetd",     _app("bin.telnetd"))
        self.register("usbmsc",      _app("bin.usbmsc"))
        self.register("mines",       _app("bin.minesweeper"))
        self.register("loracfg",     _app("bin.loracfg"))
        self.register("menu",        _app("bin.menu",       tui=True))
        self.register("nm",          _app("bin.netmgr",     tui=True))
        self.register("fm",          _app("bin.filemgr",    tui=True))
        self.register("irc",         _app("bin.irc",        tui=True))
        self.register("pop3",        _app("bin.pop3",       tui=True))
        self.register("smtp",        _app("bin.smtp",       tui=True))
        self.register("rss",         _app("bin.rss",        tui=True))
        self.register("gemini",      _app("bin.gemini",     tui=True))
        self.register("gopher",      _app("bin.gopher",     tui=True))
        self.register("dict",        _app("bin.dict",       tui=True))
        self.register("wiki",        _app("bin.wiki",       tui=True))
        self.register("epub",        _app("bin.epub",       tui=True))
        self.register("md",          _app("bin.md",         tui=True))
        self.register("fc",          _app("bin.fontcfg",    tui=True))
        self.register("play",        _app("bin.player",     tui=True, audio=True))
        self.register("stream",      _app("bin.stream",     tui=True, audio=True))
        self.register("lorachat",    _app("bin.lorachat",   tui=True, radio=True))
        self.register("blechat",     _app("bin.blechat",    tui=True, ble=True))
        self.register("rec",         _app("bin.rec",        rec=True))
        self.register("vi",          _app("modvi"))
        self.register("zm",          _app("modzm"))

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

    def _run_builtin(self, cmd_name, args):
        """Handles shell builtins that live outside the app registry
        (fav, clear, dbgrst, echo, exit, help) -- shared between the
        interactive loop and _run_rc_file() so builtins behave
        identically from both, instead of only working when typed.
        Returns True if cmd_name was one of these."""
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
            return True

        if cmd_name == "clear":
            print("\033[2J\033[H", end="")
            return True

        if cmd_name == "dbgrst":
            import machine
            import modvt
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
            # into one WDT_RESET -- modvt.reset_reason() surfaces the raw
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
            _esp_reset_cause = modvt.reset_reason()
            print("ESP reset reason: %s [%d]" % (
                _esp_reset_names.get(_esp_reset_cause, "UNKNOWN"), _esp_reset_cause))
            return True

        if cmd_name == "echo":
            print(" ".join(args))
            return True

        if cmd_name == "exit":
            self.running = False
            return True

        if cmd_name == "help":
            sorted_apps = sorted(self.apps.keys())
            print("Available commands:", ", ".join(sorted_apps))
            return True

        return False

    def _run_rc_file(self):
        """Runs startup commands from .shellrc. Checks /flash first,
        falling back to /sd"""
        for path in ("/flash/.shellrc", "/sd/.shellrc"):
            try:
                f = open(path, "r")
            except OSError:
                continue

            try:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith("#"):
                        continue
                    parts = self.parse_args(line)
                    cmd_name = parts[0]
                    args = parts[1:]
                    if self._run_builtin(cmd_name, args):
                        if not self.running:
                            break  # rc called `exit` -- stop the rest of the file too
                        continue
                    self.execute(cmd_name, *args)
            finally:
                f.close()
            break

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
            try:
                char = sys.stdin.read(1)
            except UnicodeError:
                continue

            if not char:
                continue

            if char == '\x01':
                # Ctrl-A: mpremote raw REPL request. Re-inject the byte so
                # the MicroPython C REPL loop sees it after the shell exits,
                # then exit so it can handle the raw REPL handshake.
                self.env.kvm.inject('\x01')
                raise EOFError

            if char == '\x1b':
                # Consume ESC [ A/B/C/D sequences (3 bytes total). These
                # are injected by the trackball the same way a real
                # keypress would arrive.
                nxt = sys.stdin.read(1)
                if nxt == '[':
                    seq = sys.stdin.read(1)
                    if seq == 'A':                           # UP: scroll terminal
                        self.env.term.scrollup()
                    elif seq == 'B':                         # DOWN: scroll terminal
                        self.env.term.scrolldown()
                    elif seq == 'D' and self._history:       # LEFT: older command
                        if hist_idx == len(self._history):
                            saved = buffer
                        if hist_idx > 0:
                            hist_idx -= 1
                            new_buf = self._history[hist_idx]
                            print('\b \b' * len(buffer) + new_buf, end='')
                            buffer = new_buf
                    elif seq == 'C' and self._history:       # RIGHT: newer command
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

        self.running = True

        if not self._rc_ran:
            # TODO: move versioning to makefile
            print("\x1b[2A\r\x1b[2K\x1b[38;5;45mvtOS v0.1.14.\n\x1b[2KType 'help' to see commands.\x1b[0m")
            self._rc_ran = True
            self._run_rc_file()

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

            if self._run_builtin(cmd_name, args):
                continue

            # Record in history, skipping consecutive duplicates
            if not self._history or self._history[-1] != user_input:
                if len(self._history) >= self._MAX_HISTORY:
                    self._history.pop(0)
                self._history.append(user_input)

            self.execute(cmd_name, *args)

