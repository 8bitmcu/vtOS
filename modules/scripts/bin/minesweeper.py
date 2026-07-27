#
# MicroPython Minesweeper game
# Copyright (c) 2026 8bitmcu
# License: MIT
#
import sys
import random

INVERT = "\x1b[7m"
RESET = "\x1b[0m"
HIDE_CURSOR = "\x1b[?25l"
SHOW_CURSOR = "\x1b[?25h"

COLORS = {
    '1': '\x1b[1;94m', # Bold Bright Blue
    '2': '\x1b[1;92m', # Bold Bright Green
    '3': '\x1b[1;91m', # Bold Bright Red
    '4': '\x1b[1;95m', # Bold Bright Magenta
    '5': '\x1b[1;93m', # Bold Bright Yellow
    '6': '\x1b[1;96m', # Bold Bright Cyan
    '7': '\x1b[1;97m', # Bold Bright White
    '8': '\x1b[1;37m', # Bold Standard White (Reads as a solid light gray)
    'F': '\x1b[37m',   # Standard White (Not bold, reads as soft gray)
    '*': '\x1b[1;91m', # Bold Bright Red (Bombs)
}

def cell_char(x, y, mines, revealed, flags, game_state):
    """What character belongs in a given cell right now."""
    if (x, y) in revealed or (game_state != "PLAYING" and (x, y) in mines):
        if (x, y) in mines:
            return "*"
        count = sum(1 for dx in (-1, 0, 1) for dy in (-1, 0, 1)
                    if (x + dx, y + dy) in mines and (dx != 0 or dy != 0))
        return str(count) if count > 0 else " "
    elif (x, y) in flags:
        return "F"
    else:
        return "?"

def cell_str(x, y, mines, revealed, flags, game_state, is_cursor):
    """Fully styled glyph (color + optional invert) for one cell."""
    char = cell_char(x, y, mines, revealed, flags, game_state)
    color = COLORS.get(char, "")
    if is_cursor:
        return INVERT + color + char + RESET
    return color + char + RESET

def cell_screen_pos(x, y):
    """Terminal (row, col) of the glyph inside cell (x, y). 1-indexed.

    Derived from the box-drawing layout: each cell row is followed by a
    separator row, and each cell is 4 visible columns wide (' X ' + border).
    """
    row = 2 * y + 2
    col = 4 * x + 3
    return row, col

def status_row(rows):
    return 2 * rows + 3  # matches where render_board used to print WON/LOST

def status_message(game_state):
    if game_state == "WON":
        return "\x1b[32mYOU WIN!\x1b[0m Press esc to exit."
    elif game_state == "LOST":
        return "\x1b[31mGAME OVER!\x1b[0m Press esc to exit."
    return ""

def goto(row, col):
    return "\x1b[%d;%dH" % (row, col)

def park_cursor(rows):
    # Move the real terminal cursor out of the way of the board so it
    # doesn't sit blinking on top of a cell between keypresses.
    return goto(status_row(rows) + 2, 1)

def full_board_string(cols, rows, cursor_x, cursor_y, mines, revealed, flags, game_state):
    """Builds the whole board as one string. Only used for the initial draw."""
    TL, TR, BL, BR = chr(0x250c), chr(0x2510), chr(0x2514), chr(0x2518)
    H, V, C = chr(0x2500), chr(0x2502), chr(0x253c)
    TT, BT = chr(0x252c), chr(0x2534)
    LT, RT = chr(0x251c), chr(0x2524)

    lines = [TL + (H * 3 + TT) * (cols - 1) + H * 3 + TR]

    for y in range(rows):
        row_str = V
        for x in range(cols):
            is_cursor = (x == cursor_x and y == cursor_y)
            row_str += " " + cell_str(x, y, mines, revealed, flags, game_state, is_cursor) + " " + V
        lines.append(row_str)
        if y < rows - 1:
            lines.append(LT + (H * 3 + C) * (cols - 1) + H * 3 + RT)

    lines.append(BL + (H * 3 + BT) * (cols - 1) + H * 3 + BR)
    return "\n".join(lines)

def draw_full(cols, rows, cursor_x, cursor_y, mines, revealed, flags, game_state):
    """One-time full paint: clear screen, draw the whole grid, one write() call."""
    buf = ["\x1b[2J", goto(1, 1)]
    buf.append(full_board_string(cols, rows, cursor_x, cursor_y, mines, revealed, flags, game_state))
    buf.append("\n\n")
    msg = status_message(game_state)
    if msg:
        buf.append(goto(status_row(rows), 1) + "\x1b[K" + msg)
    buf.append(park_cursor(rows))
    sys.stdout.write("".join(buf))

def draw_cells(cols, rows, cells, cursor_x, cursor_y, mines, revealed, flags, game_state):
    """Fast path: repaint only the cells that actually changed this move,
    via direct cursor addressing, batched into a single write() call."""
    buf = []
    for (x, y) in cells:
        if not (0 <= x < cols and 0 <= y < rows):
            continue
        r, c = cell_screen_pos(x, y)
        is_cursor = (x == cursor_x and y == cursor_y)
        buf.append(goto(r, c) + cell_str(x, y, mines, revealed, flags, game_state, is_cursor))

    msg = status_message(game_state)
    if msg:
        buf.append(goto(status_row(rows), 1) + "\x1b[K" + msg)

    buf.append(park_cursor(rows))
    sys.stdout.write("".join(buf))

def flood_fill(start_x, start_y, cols, rows, mines, revealed, flags):
    """Reveals empty cells and their immediate numbered borders.
    Returns the list of newly revealed cells so the caller knows exactly
    what needs to be repainted."""
    newly_revealed = []
    stack = [(start_x, start_y)]
    while stack:
        x, y = stack.pop()
        if (x, y) in revealed or (x, y) in flags:
            continue

        revealed.add((x, y))
        newly_revealed.append((x, y))

        count = sum(1 for dx in (-1, 0, 1) for dy in (-1, 0, 1)
                    if (x + dx, y + dy) in mines and (dx != 0 or dy != 0))

        if count == 0:
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < cols and 0 <= ny < rows:
                        if (nx, ny) not in revealed:
                            stack.append((nx, ny))
    return newly_revealed

def generate_mines(cols, rows, num_mines, safe_x, safe_y):
    """Places mines randomly, guaranteeing the first click area is safe."""
    mines = set()
    while len(mines) < num_mines:
        x = random.randrange(cols)
        y = random.randrange(rows)
        if abs(x - safe_x) <= 1 and abs(y - safe_y) <= 1:
            continue
        mines.add((x, y))
    return mines

def main(env, args):
    cell_width = 3
    cell_height = 1
    cols = (env.cols - 1) // (cell_width + 1)
    rows = (env.rows - 3) // (cell_height + 1)

    total_mines = max(1, int((cols * rows) * 0.15))

    mines = set()
    revealed = set()
    flags = set()
    first_click = True
    game_state = "PLAYING"
    cursor_x, cursor_y = 0, 0

    sys.stdout.write(HIDE_CURSOR)
    draw_full(cols, rows, cursor_x, cursor_y, mines, revealed, flags, game_state)

    try:
        while True:
            ch = sys.stdin.read(1)
            dirty = []  # cells that need repainting this frame

            if game_state == "PLAYING":
                if ch == 'w' and cursor_y > 0:
                    old = (cursor_x, cursor_y)
                    cursor_y -= 1
                    dirty += [old, (cursor_x, cursor_y)]
                elif ch == 's' and cursor_y < rows - 1:
                    old = (cursor_x, cursor_y)
                    cursor_y += 1
                    dirty += [old, (cursor_x, cursor_y)]
                elif ch == 'a' and cursor_x > 0:
                    old = (cursor_x, cursor_y)
                    cursor_x -= 1
                    dirty += [old, (cursor_x, cursor_y)]
                elif ch == 'd' and cursor_x < cols - 1:
                    old = (cursor_x, cursor_y)
                    cursor_x += 1
                    dirty += [old, (cursor_x, cursor_y)]

                elif ch == 'f':
                    if (cursor_x, cursor_y) not in revealed:
                        if (cursor_x, cursor_y) in flags:
                            flags.remove((cursor_x, cursor_y))
                        else:
                            flags.add((cursor_x, cursor_y))
                        dirty.append((cursor_x, cursor_y))

                elif ch == ' ' or ch == '\r' or ch == '\n':
                    if (cursor_x, cursor_y) not in flags:
                        if first_click:
                            mines = generate_mines(cols, rows, total_mines, cursor_x, cursor_y)
                            first_click = False

                        if (cursor_x, cursor_y) in mines:
                            game_state = "LOST"
                            # Reveal every mine that isn't already shown
                            dirty.extend(m for m in mines if m not in revealed)
                        else:
                            dirty.extend(flood_fill(cursor_x, cursor_y, cols, rows, mines, revealed, flags))
                            if len(revealed) == (cols * rows) - total_mines:
                                game_state = "WON"

            if ch == '\x1b':
                sys.stdout.write(SHOW_CURSOR + "\nExiting...\n")
                break

            if dirty:
                draw_cells(cols, rows, dirty, cursor_x, cursor_y, mines, revealed, flags, game_state)
    finally:
        sys.stdout.write(SHOW_CURSOR)
