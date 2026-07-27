
#
# MicroPython TUI command shortcut menu
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import sys
import json

def save_menu(json_file, json_data):
    try:
        with open(json_file, "w") as f:
            json.dump(json_data, f)
    except OSError as e:
        print(f"Failed to save json file: {e}")

def main(env, args):
    """ Creates a TUI for browsing command shortcuts """

    app_name = args[0] if len(args) > 0 else None

    if not app_name:
        print("Usage: menu <command>")
        return

    tui = env.tui
    ui_state = "MAIN_MENU"
    tui.enter_altscreen()
    tui.cursor_hide()

    app = {}
    app_file = f"/flash/menu/.{app_name}.json"

    try:
        with open(app_file, "r") as f:
            app = json.load(f)
    except (OSError, ValueError):
        pass

    while True:
        win = tui.make_window(
                0, 0,
                width=env.cols, height=env.rows,
                title=app_name.upper() + " MENU",
                fg=252, bg=18)

        if ui_state == "MAIN_MENU":
            status = win.make_label("[w/s] nav | [n]ew | [d]elete | [q]uit",
                                    0, win.inner_h-1,
                                    fg=0, bg=252,
                                    width=win.inner_w)

            lst = win.make_list(sorted(list(app.keys())),
                                x=0, y=0,
                                width=env.cols, height=win.inner_h-1,
                                fg=252, bg=18,
                                arrow=">", left_pad=1)

            win.invalidate()
            while True:
                win.draw()
                lst.draw()
                status.draw()
                tui.draw()

                char = sys.stdin.read(1)
                if char == "\n" or char == "\r":
                    value = app.get(lst.value, "")
                    args = env.shell.parse_args(value)
                    tui.clear_screen()

                    if env.shell.execute(app_name, *args):
                        tui.enter_altscreen()
                        tui.cursor_hide()
                    else:
                        print("\n[Press any key to return to menu]")
                        sys.stdin.read(1)
                        tui.enter_altscreen()
                        tui.cursor_hide()
                    break
                elif char == "n":
                    ui_state = "NEW_ENTRY"
                    break
                elif char == "d":
                    dlg = win.make_dialog(f"Delete this {app_name} entry?",
                                          x=2, y=win.inner_h//2 - 2,
                                          width=win.inner_w - 4, height=5,
                                          fg=252, bg=18,
                                          btn1="Yes", btn2="No",
                                          sel_fg=0, sel_bg=252,
                                          title="CONFIRM")

                    status = win.make_label("[w/s] nav | [enter] accept",
                                            0, win.inner_h-1,
                                            fg=0, bg=252,
                                            width=win.inner_w)

                    win.invalidate()
                    while True:
                        win.draw()
                        dlg.draw()
                        status.draw()
                        tui.draw()

                        char = sys.stdin.read(1)
                        if char in ('\r', '\n'):
                            if dlg.selected == 0:  # "Yes"
                                del app[lst.value]
                                save_menu(app_file, app)
                            break
                        elif char == 'w':
                            dlg.left()
                        elif char == 's':
                            dlg.right()
                    break
                elif char == "w":
                    lst.up()
                elif char == "s":
                    lst.down()
                elif char == "q":
                    ui_state = "QUIT"
                    break

        elif ui_state == "NEW_ENTRY":
            new_lbl = win.make_label(f"Add new {app_name}: ",
                                       0, 1,
                                       fg=252, bg=18,
                                       align="center")

            name_input = win.make_input("Name:",
                                 0, 3,
                                 width=win.inner_w-2,
                                 fg=252, bg=18, input_bg=0,
                                 decorations=False,
                                 align="center")

            args_input = win.make_input("Args:",
                                 0, 6,
                                 width=win.inner_w-2,
                                 fg=252, bg=18, input_bg=0,
                                 decorations=False,
                                 align="center")

            status = win.make_label("[esc] back | [enter] next",
                                    0, win.inner_h-1,
                                    fg=0, bg=252,
                                    width=win.inner_w)
            tui.cursor_show()
            win.invalidate()
            win.draw()
            new_lbl.draw()
            status.draw()
            args_input.draw()
            name_input.draw()

            while ui_state == "NEW_ENTRY":
                char = sys.stdin.read(1)
                if char in ('\r', '\n'):
                    name = name_input.value

                    while True:
                        args_input.draw()
                        tui.draw()

                        char = sys.stdin.read(1)
                        if char in ('\r', '\n'):
                            app[name] = args_input.value
                            save_menu(app_file, app)

                            tui.cursor_hide()
                            ui_state = "MAIN_MENU"
                            break

                        elif char in ('\x08', '\x7f'):  # backspace
                            args_input.backspace()
                        elif char == '\x1b':            # escape: back to name field
                            break
                        else:
                            args_input.push(char)
                        args_input.draw()
                        tui.draw()

                elif char in ('\x08', '\x7f'):  # backspace
                    name_input.backspace()
                elif char == '\x1b':            # escape: back to main menu
                    tui.cursor_hide()
                    ui_state = "MAIN_MENU"
                    break
                else:
                    name_input.push(char)
                name_input.draw()
                tui.draw()

        elif ui_state == "QUIT":
            tui.exit_altscreen()
            tui.cursor_show()
            return

