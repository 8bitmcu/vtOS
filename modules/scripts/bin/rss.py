#
# MicroPython TUI RSS Reader
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import gc
import sys
import modxml
import requests

def _find_all(node, tag, out):
    """ Recursively collects every element (at any depth) with the given
    tag name, depth-first -- <item> sits a couple of levels below the
    <rss> root (<rss><channel><item>...), not as a direct child. """
    if node["tag"] == tag:
        out.append(node)
    for child in node["children"]:
        _find_all(child, tag, out)

def _find_text(node, tag):
    """ Returns the text of the first direct child with the given tag
    name, or "" if absent -- <title>/<description> are always direct
    children of <item>, never nested further. """
    for child in node["children"]:
        if child["tag"] == tag:
            return child["text"]
    return ""

def main(env, args):
    """ Creates a TUI for browsing and reading RSS feeds """

    host = args[0] if len(args) > 0 else None
    if not host:
        print("Usage: rss <URL>")
        return

    tui = env.tui
    ui_state = "MAIN_MENU"
    tui.enter_altscreen()
    tui.cursor_hide()
    story = None

    win = tui.make_window(
            0, 0,
            width=env.cols, height=env.rows,
            title="RSS READER",
            fg=252, bg=18)

    win.invalidate()
    win.draw()
    tui.draw()
    win.draw_label(f"Loading...",
                   0, win.inner_h // 2,
                   fg=252, bg=18,
                   align="center")

    titles = []
    items = []
    try:
        response = requests.get(host)
        rss_data = response.text

        tree = modxml.parse(rss_data)
        if tree is not None:
            _find_all(tree, "item", items)

        for item in items:
            titles.append("- " + (_find_text(item, "title") or "N/A"))

    except Exception as e:
        tui.exit_altscreen()
        tui.cursor_show()
        print(f"Error: {e}")
        return
    finally:
        response.close()

    while True:

        if ui_state == "MAIN_MENU":

            status = win.make_label("[w/s] nav | [enter] read | [q]uit",
                                    0, win.inner_h-1,
                                    fg=0, bg=252,
                                    width=win.inner_w)

            lst = win.make_list(titles,
                                x=0, y=0,
                                width=env.cols, height=win.inner_h-1,
                                fg=252, bg=18,
                                arrow=">", left_pad=1,
                                multiline=True, wrap=True)

            win.invalidate()
            while True:
                win.draw()
                lst.draw()
                status.draw()
                tui.draw()

                char = sys.stdin.read(1)
                if char == "\n" or char == "\r":
                    story = lst.index
                    ui_state = "READ_STORY"
                    break
                if char == "w":
                    lst.up()
                elif char == "s":
                    lst.down()
                elif char == "q":
                    ui_state = "QUIT"
                    break

        elif ui_state == "READ_STORY":
            desc = _find_text(items[story], "description") or "No description available."

            lbl = win.make_block(desc.strip(), 0, 0,
                  width=win.inner_w, height=win.inner_h-1,
                  fg=252, bg=18,
                  scroll=True, wrap=True)

            status = win.make_label("[b]ack | [q]uit",
                                    0, win.inner_h-1,
                                    fg=0, bg=252,
                                    width=win.inner_w)

            win.invalidate()
            while True:
                win.draw()
                lbl.draw()
                status.draw()
                tui.draw()

                char = sys.stdin.read(1)
                if char == "b":
                    ui_state = "MAIN_MENU"
                    break
                if char == "q":
                    ui_state = "QUIT"
                    break

        elif ui_state == "QUIT":
            gc.collect()
            tui.exit_altscreen()
            tui.cursor_show()
            return
