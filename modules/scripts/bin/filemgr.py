#
# MicroPython TUI File Manager
# Copyright (c) 2026 8bitmcu
# License: MIT
#
import os
import sys

file_association = {
    "dat": "zm",
    "z3": "zm",
    "z5": "zm",
    "mp3": "play",
    "wav": "play",
    "c2": "play",
}

def fmt_size(size):
    if size < 1024:
        return f"{size}B"
    elif size < 1024 * 1024:
        return f"{size / 1024:.1f}K"
    elif size < 1024 * 1024 * 1024:
        return f"{size / (1024 * 1024):.1f}M"
    else:
        return f"{size / (1024 * 1024 * 1024):.1f}G"

def get_extension(path):
    dot_index = path.rfind('.')
    if dot_index > 0:
        return path[dot_index + 1:]
    return ""

def copy_file(src, dst):
    with open(src, 'rb') as f_in:
        with open(dst, 'wb') as f_out:
            while True:
                chunk = f_in.read(512)
                if not chunk:
                    break
                f_out.write(chunk)

def joinpath(d, name):
    return (d + "/" + name).replace("//", "/")

def run_script(path, env, tui):
    """Executes an arbitrary .py file, giving it `env` as a global so it
    can reach the same terminal/audio/shell state any registered app
    gets. Unlike file_association apps (which are curated, TUI-aware
    apps that manage their own screen), an ad hoc script is likely just
    a few print() statements -- pause for a keypress afterward so that
    output (or a traceback) is actually visible before wiping back to
    the file manager."""
    tui.cursor_show()
    tui.clear_screen()
    try:
        with open(path) as f:
            src = f.read()
        exec(src, {"env": env, "__name__": "__main__"})
    except Exception as e:
        sys.print_exception(e)
    print("\n[Press any key to continue]")
    sys.stdin.read(1)
    tui.cursor_hide()
    tui.enter_altscreen()

def show_error(win, tui, msg):
    dlg = win.make_dialog(msg,
                          x=2, y=win.inner_h//2-2,
                          width=win.inner_w-4, height=5,
                          fg=252, bg=18,
                          btn1="OK",
                          sel_fg=0, sel_bg=252,
                          title="ERROR")
    win.invalidate()
    while True:
        win.draw()
        dlg.draw()
        tui.draw()
        char = sys.stdin.read(1)
        if char in ('\r', '\n'):
            break

def get_parent(current_dir):
    if current_dir != "/":
        parts = current_dir.split('/')
        new_path = "/".join(parts[:-1])
        return new_path if new_path else "/"
    return current_dir

def main(env, args):
    """ Creates a TUI for browsing and manipulating files """

    tui = env.tui
    ui_state = "MAIN_MENU"
    tui.enter_altscreen()
    tui.cursor_hide()
    current_dir = "/"

    CLR  = "\x1b[0m"
    BOLD = "\x1b[1m"
    CYAN = "\x1b[38;5;45m"

    win = tui.make_window(
            0, 0,
            width=env.cols, height=env.rows,
            title="FILE MANAGER",
            fg=252, bg=18)

    while True:
        if ui_state == "MAIN_MENU":

            blk = win.make_block(f"{BOLD}cwd: {CLR}{CYAN}{current_dir}{CLR}\n",
                                 1, 0,
                                 fg=252, bg=18)

            status = win.make_label("[w/s] nav | [h]elp | [b]ack | [q]uit",
                                    0, win.inner_h-1,
                                    fg=0, bg=252,
                                    width=win.inner_w)

            dirs = []
            files = []
            for name in os.listdir(current_dir):
                try:
                    if os.stat(joinpath(current_dir, name))[0] & 0x4000:
                        dirs.append(name)
                    else:
                        files.append(name)
                except OSError:
                    files.append(name)
            dirs.sort()
            files.sort()
            items = dirs + files
            display_items = [n + "/" for n in dirs] + files
            if current_dir != "/":
                items.insert(0, "..")
                display_items.insert(0, "..")

            lst = win.make_list(display_items,
                                x=0, y=1,
                                width=env.cols, height=win.inner_h-2,
                                fg=252, bg=18,
                                arrow=">", left_pad=1)

            win.invalidate()
            while True:
                win.draw()
                blk.draw()
                lst.draw()
                status.draw()
                tui.draw()

                # Empty directory: only allow quit/back/help
                if not items:
                    char = sys.stdin.read(1)
                    if char == "h":
                        ui_state = "HELP"
                        break
                    elif char == "b":
                        current_dir = get_parent(current_dir)
                        break
                    elif char == "q":
                        ui_state = "QUIT"
                        break
                    continue

                char = sys.stdin.read(1)
                if char in ('\r', '\n'): # enter key
                    selected_item = items[lst.selected]
                    if selected_item == "..":
                        current_dir = get_parent(current_dir)
                    else:
                        new_path = joinpath(current_dir, selected_item)
                        try:
                            mode = os.stat(new_path)[0]
                            if mode & 0x4000:
                                current_dir = new_path
                            else:
                                ext = get_extension(new_path)
                                if ext == "py":
                                    run_script(new_path, env, tui)
                                elif ext in file_association:
                                    cmd = file_association[ext]
                                    tui.cursor_show()
                                    tui.clear_screen()
                                    env.shell.execute(cmd, new_path)
                                    tui.cursor_hide()
                                    tui.enter_altscreen()
                                else:
                                    show_error(win, tui, f"no application for .{ext} files")
                        except OSError:
                            pass
                    break

                elif char == "e":    # edit
                    selected_item = items[lst.selected]
                    if selected_item != "..":
                        new_path = joinpath(current_dir, selected_item)
                        try:
                            mode = os.stat(new_path)[0]
                            if mode & 0x4000:
                                show_error(win, tui, "Cannot edit directories")
                            else:
                                tui.cursor_show()
                                tui.clear_screen()
                                env.shell.execute("vi", new_path)
                                tui.cursor_hide()
                                tui.enter_altscreen()
                        except OSError:
                            pass
                    break

                elif char == "w":
                    lst.up()

                elif char == "s":
                    lst.down()

                elif char == "b":
                    current_dir = get_parent(current_dir)
                    break

                elif char == "i":    # file/dir info
                    selected_item = items[lst.selected]
                    if selected_item == "..":
                        continue
                    fpath = joinpath(current_dir, selected_item)
                    try:
                        mode, ino, dev, nlink, uid, gid, size, atime, mtime, ctime = os.stat(fpath)
                    except OSError as e:
                        show_error(win, tui, f"stat failed: {e}")
                        break
                    ftype = "folder" if mode & 0x4000 else "file"

                    blk = win.make_block(f"{BOLD}{fpath}\n"
                                         f"\n"
                                         f"  {BOLD}Type :{CLR}  {ftype}\n"
                                         f"  {BOLD}Size :{CLR}  {fmt_size(size)}",
                                         1, 1,
                                         fg=252, bg=18)

                    status = win.make_label("[b]ack | [q]uit",
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
                        if char == "b":
                            break
                        elif char == "q":
                            ui_state = "QUIT"
                            break

                    break  # re-initialize main menu

                elif char == "n":    # new directory
                    lbl_new = win.make_label("New directory:",
                                             0, 1,
                                             fg=252, bg=18,
                                             align="center")

                    name = win.make_input("Name:",
                                         0, win.inner_h//2,
                                         width=win.inner_w-2,
                                         fg=252, bg=18, input_bg=0,
                                         align="center")

                    tui.cursor_show()
                    win.invalidate()
                    while True:
                        win.draw()
                        blk.draw()
                        lst.draw()
                        lbl_new.draw()
                        name.draw()
                        status.draw()
                        tui.draw()

                        char = sys.stdin.read(1)

                        if char in ('\r', '\n'):
                            tui.cursor_hide()
                            try:
                                os.mkdir(joinpath(current_dir, name.value))
                            except OSError as e:
                                show_error(win, tui, f"mkdir failed: {e}")
                            break
                        elif char in ('\x08', '\x7f'):
                            name.backspace()
                        elif char == '\x1b':
                            tui.cursor_hide()
                            break
                        else:
                            name.push(char)
                    break  # re-initialize main menu

                elif char == "r":    # rename
                    selected_item = items[lst.selected]
                    if selected_item == "..":
                        continue

                    lbl_rename = win.make_label("Rename to:",
                                                0, 1,
                                                fg=252, bg=18,
                                                align="center")

                    rename = win.make_input("Name:",
                                         0, win.inner_h//2,
                                         width=win.inner_w-2,
                                         fg=252, bg=18, input_bg=0,
                                         value=selected_item,
                                         align="center")

                    tui.cursor_show()
                    win.invalidate()
                    while True:
                        win.draw()
                        blk.draw()
                        lst.draw()
                        lbl_rename.draw()
                        rename.draw()
                        status.draw()
                        tui.draw()

                        char = sys.stdin.read(1)

                        if char in ('\r', '\n'):
                            tui.cursor_hide()
                            try:
                                os.rename(joinpath(current_dir, selected_item),
                                          joinpath(current_dir, rename.value))
                            except OSError as e:
                                show_error(win, tui, f"rename failed: {e}")
                            break
                        elif char in ('\x08', '\x7f'):
                            rename.backspace()
                        elif char == '\x1b':
                            tui.cursor_hide()
                            break
                        else:
                            rename.push(char)
                    break  # re-initialize main menu

                elif char == "d":    # delete
                    selected_item = items[lst.selected]
                    if selected_item == "..":
                        continue

                    fpath = joinpath(current_dir, selected_item)
                    try:
                        is_dir = bool(os.stat(fpath)[0] & 0x4000)
                    except OSError as e:
                        show_error(win, tui, f"stat failed: {e}")
                        break

                    dlg = win.make_dialog("Delete this folder?" if is_dir else "Delete this file?",
                                          x=2, y=win.inner_h//2 - 2,
                                          width=win.inner_w - 4, height=5,
                                          fg=252, bg=18,
                                          btn1="Yes", btn2="No",
                                          sel_fg=0, sel_bg=252,
                                          title="CONFIRM")

                    win.invalidate()
                    while True:
                        win.draw()
                        dlg.draw()
                        tui.draw()

                        char = sys.stdin.read(1)
                        if char in ('\r', '\n'):
                            if dlg.selected == 0:  # "Yes"
                                try:
                                    if is_dir:
                                        os.rmdir(fpath)
                                    else:
                                        os.remove(fpath)
                                except OSError as e:
                                    show_error(win, tui, f"delete failed: {e}")
                            break
                        elif char == 'w':
                            dlg.left()
                        elif char == 's':
                            dlg.right()
                    break  # re-initialize main menu

                elif char == "m":    # move
                    selected_item = items[lst.selected]
                    if selected_item == "..":
                        continue

                    src = joinpath(current_dir, selected_item)
                    move_input = win.make_input("Destination:",
                                               0, win.inner_h//2,
                                               width=win.inner_w-2,
                                               fg=252, bg=18, input_bg=0,
                                               value=src,
                                               align="center")

                    lbl_move = win.make_label("Move to:",
                                              0, 1,
                                              fg=252, bg=18,
                                              align="center")

                    tui.cursor_show()
                    win.invalidate()
                    while True:
                        win.draw()
                        blk.draw()
                        lst.draw()
                        lbl_move.draw()
                        move_input.draw()
                        status.draw()
                        tui.draw()

                        char = sys.stdin.read(1)

                        if char in ('\r', '\n'):
                            tui.cursor_hide()
                            try:
                                os.rename(src, move_input.value)
                            except OSError as e:
                                show_error(win, tui, f"move failed: {e}")
                            break
                        elif char in ('\x08', '\x7f'):
                            move_input.backspace()
                        elif char == '\x1b':
                            tui.cursor_hide()
                            break
                        else:
                            move_input.push(char)
                    break  # re-initialize main menu

                elif char == "c":    # copy
                    selected_item = items[lst.selected]
                    if selected_item == "..":
                        continue

                    src = joinpath(current_dir, selected_item)
                    try:
                        is_dir = bool(os.stat(src)[0] & 0x4000)
                    except OSError as e:
                        show_error(win, tui, f"stat failed: {e}")
                        break

                    if is_dir:
                        show_error(win, tui, "Cannot copy directories")
                        break

                    copy_input = win.make_input("Destination:",
                                               0, win.inner_h//2,
                                               width=win.inner_w-2,
                                               fg=252, bg=18, input_bg=0,
                                               value=src,
                                               align="center")

                    lbl_copy = win.make_label("Copy to:",
                                              0, 1,
                                              fg=252, bg=18,
                                              align="center")

                    tui.cursor_show()
                    win.invalidate()
                    while True:
                        win.draw()
                        blk.draw()
                        lst.draw()
                        lbl_copy.draw()
                        copy_input.draw()
                        status.draw()
                        tui.draw()

                        char = sys.stdin.read(1)

                        if char in ('\r', '\n'):
                            tui.cursor_hide()
                            try:
                                copy_file(src, copy_input.value)
                            except OSError as e:
                                show_error(win, tui, f"copy failed: {e}")
                            break
                        elif char in ('\x08', '\x7f'):
                            copy_input.backspace()
                        elif char == '\x1b':
                            tui.cursor_hide()
                            break
                        else:
                            copy_input.push(char)
                    break  # re-initialize main menu

                elif char == "h":
                    ui_state = "HELP"
                    break
                elif char == "q":
                    ui_state = "QUIT"
                    break

        elif ui_state == "HELP":
            blk = win.make_block(f"{BOLD}w: {CLR}move up\n"
                                 f"{BOLD}s: {CLR}move down\n"
                                 f"{BOLD}b: {CLR}move up one directory\n"
                                 f"{BOLD}i: {CLR}file or directory info\n"
                                 f"{BOLD}m: {CLR}move file or directory\n"
                                 f"{BOLD}c: {CLR}copy file or directory\n"
                                 f"{BOLD}r: {CLR}rename file or directory\n"
                                 f"{BOLD}d: {CLR}delete file or directory\n"
                                 f"{BOLD}n: {CLR}create new directory\n"
                                 f"{BOLD}e: {CLR}edit file in vi\n",
                                 1, 0,
                                 fg=252, bg=18)

            status = win.make_label("[b]ack | [q]uit",
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

                if char == "b":
                    ui_state = "MAIN_MENU"
                    break
                elif char == "q":
                    ui_state = "QUIT"
                    break

        elif ui_state == "QUIT":
            tui.exit_altscreen()
            tui.cursor_show()
            return

