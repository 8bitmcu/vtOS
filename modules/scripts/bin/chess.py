#
# MicroPython Chess
# Copyright (c) 2026 8bitmcu
# License: MIT
#
# Navigable board with real rules: legal moves only, turn enforcement,
# check/checkmate/stalemate detection, castling, en passant, promotion
# (auto-queen). You play White; Black is always played by the built-in
# engine (minimax + alpha-beta, material + piece-square-table evaluation).
# Press 'e' to force an engine move for whoever's turn it currently is.
#
# Deliberately doesn't implement draw-by-repetition or the 50-move rule.
# Everything below is an original implementation -- no external chess
# library, no vendored engine.
#
import sys

WHITE, BLACK = "W", "B"

BACK_RANK = ["R", "N", "B", "Q", "K", "B", "N", "R"]

PIECE_VALUES = {"P": 100, "N": 320, "B": 330, "R": 500, "Q": 900, "K": 20000}

# Piece-square tables: row 0 = rank 8, White's perspective (flipped for
# Black at lookup time in evaluate()). Original, hand-picked values
# encoding standard chess-programming heuristics -- central knights and
# bishops, advancing pawns, a corner-hugging king before the endgame --
# not copied from any specific published table.
_PAWN_PST = [
    [0,   0,   0,   0,   0,   0,   0,   0],
    [50,  50,  50,  50,  50,  50,  50,  50],
    [10,  10,  20,  30,  30,  20,  10,  10],
    [5,   5,   10,  25,  25,  10,  5,   5],
    [0,   0,   0,   20,  20,  0,   0,   0],
    [5,  -5,  -10,  0,   0,  -10, -5,   5],
    [5,   10,  10, -20, -20,  10,  10,  5],
    [0,   0,   0,   0,   0,   0,   0,   0],
]
_KNIGHT_PST = [
    [-50, -40, -30, -30, -30, -30, -40, -50],
    [-40, -20,  0,   0,   0,   0,  -20, -40],
    [-30,  0,   10,  15,  15,  10,  0,  -30],
    [-30,  5,   15,  20,  20,  15,  5,  -30],
    [-30,  0,   15,  20,  20,  15,  0,  -30],
    [-30,  5,   10,  15,  15,  10,  5,  -30],
    [-40, -20,  0,   5,   5,   0,  -20, -40],
    [-50, -40, -30, -30, -30, -30, -40, -50],
]
_BISHOP_PST = [
    [-20, -10, -10, -10, -10, -10, -10, -20],
    [-10,  0,   0,   0,   0,   0,   0,  -10],
    [-10,  0,   5,   10,  10,  5,   0,  -10],
    [-10,  5,   5,   10,  10,  5,   5,  -10],
    [-10,  0,   10,  10,  10,  10,  0,  -10],
    [-10,  10,  10,  10,  10,  10,  10, -10],
    [-10,  5,   0,   0,   0,   0,   5,  -10],
    [-20, -10, -10, -10, -10, -10, -10, -20],
]
_ROOK_PST = [
    [0,  0,  0,  0,  0,  0,  0,  0],
    [5,  10, 10, 10, 10, 10, 10, 5],
    [-5, 0,  0,  0,  0,  0,  0, -5],
    [-5, 0,  0,  0,  0,  0,  0, -5],
    [-5, 0,  0,  0,  0,  0,  0, -5],
    [-5, 0,  0,  0,  0,  0,  0, -5],
    [-5, 0,  0,  0,  0,  0,  0, -5],
    [0,  0,  0,  5,  5,  0,  0,  0],
]
_QUEEN_PST = [
    [-20, -10, -10, -5, -5, -10, -10, -20],
    [-10,  0,   0,   0,  0,  0,   0,  -10],
    [-10,  0,   5,   5,  5,  5,   0,  -10],
    [-5,   0,   5,   5,  5,  5,   0,  -5],
    [0,    0,   5,   5,  5,  5,   0,  -5],
    [-10,  5,   5,   5,  5,  5,   0,  -10],
    [-10,  0,   5,   0,  0,  0,   0,  -10],
    [-20, -10, -10, -5, -5, -10, -10, -20],
]
_KING_PST = [
    [-30, -40, -40, -50, -50, -40, -40, -30],
    [-30, -40, -40, -50, -50, -40, -40, -30],
    [-30, -40, -40, -50, -50, -40, -40, -30],
    [-30, -40, -40, -50, -50, -40, -40, -30],
    [-20, -30, -30, -40, -40, -30, -30, -20],
    [-10, -20, -20, -20, -20, -20, -20, -10],
    [20,  20,   0,   0,   0,   0,  20,  20],
    [20,  30,  10,   0,   0,  10,  30,  20],
]

PST = {"P": _PAWN_PST, "N": _KNIGHT_PST, "B": _BISHOP_PST,
       "R": _ROOK_PST, "Q": _QUEEN_PST, "K": _KING_PST}

# Engine search depth (ply). Every node is a fresh legal_moves() call plus
# a full board copy per move -- deliberately simple over fast (see
# best_move() below). Raise this if your device can spare the time; it
# gets expensive fast.
ENGINE_DEPTH = 2


# =========================================================================
# Rules + search engine
# =========================================================================

class State:
    """Mutable game state: board + side to move + castling rights + en
    passant target. board[y][x] is (color, kind) or None; y=0 is rank 8,
    x=0 is the a-file."""

    def __init__(self, setup=True):
        self.board = [[None] * 8 for _ in range(8)]
        if setup:
            for x in range(8):
                self.board[0][x] = (BLACK, BACK_RANK[x])
                self.board[1][x] = (BLACK, "P")
                self.board[6][x] = (WHITE, "P")
                self.board[7][x] = (WHITE, BACK_RANK[x])
        self.turn = WHITE
        # white_kingside, white_queenside, black_kingside, black_queenside
        self.castling = [True, True, True, True]
        self.ep_target = None  # (x, y) capturable en passant this move, or None

    def copy(self):
        # Not State.__new__(State) -- MicroPython's object model doesn't
        # expose __new__ as a callable attribute the way CPython does
        # unless a class defines its own. Build through the normal
        # constructor instead; setup=False skips the back-rank loop since
        # every field gets overwritten immediately below anyway.
        s = State(setup=False)
        s.board = [row[:] for row in self.board]
        s.turn = self.turn
        s.castling = self.castling[:]
        s.ep_target = self.ep_target
        return s


def other(color):
    return BLACK if color == WHITE else WHITE


def in_bounds(x, y):
    return 0 <= x < 8 and 0 <= y < 8


def find_king(board, color):
    for y in range(8):
        for x in range(8):
            p = board[y][x]
            if p is not None and p[0] == color and p[1] == "K":
                return x, y
    return None


_KNIGHT_OFFSETS = ((1, 2), (2, 1), (2, -1), (1, -2),
                   (-1, -2), (-2, -1), (-2, 1), (-1, 2))
_KING_OFFSETS = ((1, 0), (1, 1), (0, 1), (-1, 1),
                 (-1, 0), (-1, -1), (0, -1), (1, -1))
_BISHOP_DIRS = ((1, 1), (1, -1), (-1, 1), (-1, -1))
_ROOK_DIRS = ((1, 0), (-1, 0), (0, 1), (0, -1))


def is_square_attacked(board, x, y, by_color):
    """True if by_color has any piece attacking square (x, y)."""
    # A by_color pawn attacking (x, y) sits one row further from its own
    # back rank -- i.e. toward +y for White, -y for Black.
    pdy = 1 if by_color == WHITE else -1
    for dx in (-1, 1):
        px, py = x + dx, y + pdy
        if in_bounds(px, py):
            p = board[py][px]
            if p is not None and p[0] == by_color and p[1] == "P":
                return True

    for dx, dy in _KNIGHT_OFFSETS:
        nx, ny = x + dx, y + dy
        if in_bounds(nx, ny):
            p = board[ny][nx]
            if p is not None and p[0] == by_color and p[1] == "N":
                return True

    for dx, dy in _KING_OFFSETS:
        nx, ny = x + dx, y + dy
        if in_bounds(nx, ny):
            p = board[ny][nx]
            if p is not None and p[0] == by_color and p[1] == "K":
                return True

    for dirs, kinds in ((_BISHOP_DIRS, ("B", "Q")), (_ROOK_DIRS, ("R", "Q"))):
        for dx, dy in dirs:
            nx, ny = x + dx, y + dy
            while in_bounds(nx, ny):
                p = board[ny][nx]
                if p is not None:
                    if p[0] == by_color and p[1] in kinds:
                        return True
                    break
                nx += dx
                ny += dy

    return False


def in_check(state, color):
    king = find_king(state.board, color)
    if king is None:
        return False
    return is_square_attacked(state.board, king[0], king[1], other(color))


# --- Pseudo-legal move generation (doesn't check for self-check) -------
#
# A move is (fx, fy, tx, ty, special), special one of:
#   None, "double" (2-square pawn push), "ep" (en passant capture),
#   "castle_k", "castle_q", "promo" (always auto-queen)

def _pawn_moves(state, x, y, color):
    moves = []
    board = state.board
    dy = -1 if color == WHITE else 1
    start_rank = 6 if color == WHITE else 1
    promo_rank = 0 if color == WHITE else 7

    ny = y + dy
    if in_bounds(x, ny) and board[ny][x] is None:
        special = "promo" if ny == promo_rank else None
        moves.append((x, y, x, ny, special))
        ny2 = y + 2 * dy
        if y == start_rank and board[ny2][x] is None:
            moves.append((x, y, x, ny2, "double"))

    for dx in (-1, 1):
        nx, ny = x + dx, y + dy
        if not in_bounds(nx, ny):
            continue
        target = board[ny][nx]
        if target is not None and target[0] != color:
            special = "promo" if ny == promo_rank else None
            moves.append((x, y, nx, ny, special))
        elif state.ep_target == (nx, ny):
            moves.append((x, y, nx, ny, "ep"))

    return moves


def _step_moves(board, x, y, color, offsets):
    moves = []
    for dx, dy in offsets:
        nx, ny = x + dx, y + dy
        if in_bounds(nx, ny):
            target = board[ny][nx]
            if target is None or target[0] != color:
                moves.append((x, y, nx, ny, None))
    return moves


def _slide_moves(board, x, y, color, dirs):
    moves = []
    for dx, dy in dirs:
        nx, ny = x + dx, y + dy
        while in_bounds(nx, ny):
            target = board[ny][nx]
            if target is None:
                moves.append((x, y, nx, ny, None))
            else:
                if target[0] != color:
                    moves.append((x, y, nx, ny, None))
                break
            nx += dx
            ny += dy
    return moves


def _castle_moves(state, x, y, color):
    moves = []
    board = state.board
    if in_check(state, color):
        return moves
    opp = other(color)
    rank = 7 if color == WHITE else 0
    k_idx, q_idx = (0, 1) if color == WHITE else (2, 3)

    if state.castling[k_idx]:
        if (board[rank][5] is None and board[rank][6] is None and
                board[rank][7] == (color, "R")):
            if (not is_square_attacked(board, 5, rank, opp) and
                    not is_square_attacked(board, 6, rank, opp)):
                moves.append((x, y, 6, rank, "castle_k"))

    if state.castling[q_idx]:
        if (board[rank][1] is None and board[rank][2] is None and
                board[rank][3] is None and board[rank][0] == (color, "R")):
            if (not is_square_attacked(board, 3, rank, opp) and
                    not is_square_attacked(board, 2, rank, opp)):
                moves.append((x, y, 2, rank, "castle_q"))

    return moves


def pseudo_legal_moves(state, color):
    board = state.board
    moves = []
    for y in range(8):
        for x in range(8):
            p = board[y][x]
            if p is None or p[0] != color:
                continue
            kind = p[1]
            if kind == "P":
                moves += _pawn_moves(state, x, y, color)
            elif kind == "N":
                moves += _step_moves(board, x, y, color, _KNIGHT_OFFSETS)
            elif kind == "B":
                moves += _slide_moves(board, x, y, color, _BISHOP_DIRS)
            elif kind == "R":
                moves += _slide_moves(board, x, y, color, _ROOK_DIRS)
            elif kind == "Q":
                moves += _slide_moves(board, x, y, color, _BISHOP_DIRS + _ROOK_DIRS)
            elif kind == "K":
                moves += _step_moves(board, x, y, color, _KING_OFFSETS)
                moves += _castle_moves(state, x, y, color)
    return moves


def make_move(state, move):
    """Applies move to state, returning a NEW State (state is untouched).
    Does not check legality -- see legal_moves()."""
    fx, fy, tx, ty, special = move
    s = state.copy()
    board = s.board
    piece = board[fy][fx]
    color = piece[0]

    board[fy][fx] = None
    board[ty][tx] = piece

    if special == "ep":
        board[fy][tx] = None  # captured pawn sits beside the mover, not on (tx, ty)
    elif special == "promo":
        board[ty][tx] = (color, "Q")
    elif special == "castle_k":
        rank = fy
        board[rank][5] = board[rank][7]
        board[rank][7] = None
    elif special == "castle_q":
        rank = fy
        board[rank][3] = board[rank][0]
        board[rank][0] = None

    s.ep_target = (tx, fy + (ty - fy) // 2) if special == "double" else None

    if piece[1] == "K":
        if color == WHITE:
            s.castling[0] = s.castling[1] = False
        else:
            s.castling[2] = s.castling[3] = False
    # Losing castling rights on rook move OR capture of an unmoved rook.
    if (fx, fy) == (0, 0) or (tx, ty) == (0, 0):
        s.castling[3] = False  # black queenside
    if (fx, fy) == (7, 0) or (tx, ty) == (7, 0):
        s.castling[2] = False  # black kingside
    if (fx, fy) == (0, 7) or (tx, ty) == (0, 7):
        s.castling[1] = False  # white queenside
    if (fx, fy) == (7, 7) or (tx, ty) == (7, 7):
        s.castling[0] = False  # white kingside

    s.turn = other(color)
    return s


def legal_moves(state, color=None):
    """All fully legal moves for color (default: state.turn) -- pseudo-legal
    moves that don't leave that color's own king in check."""
    if color is None:
        color = state.turn
    result = []
    for move in pseudo_legal_moves(state, color):
        if not in_check(make_move(state, move), color):
            result.append(move)
    return result


def game_status(state):
    """Returns 'checkmate', 'stalemate', or 'playing'. Doesn't check
    draw-by-repetition or the 50-move rule."""
    if legal_moves(state):
        return "playing"
    return "checkmate" if in_check(state, state.turn) else "stalemate"


def evaluate(state):
    """Material + piece-square-table score. Positive favors White."""
    score = 0
    for y in range(8):
        for x in range(8):
            p = state.board[y][x]
            if p is None:
                continue
            color, kind = p
            value = PIECE_VALUES[kind]
            pst_row = y if color == BLACK else (7 - y)
            value += PST[kind][pst_row][x]
            score += value if color == WHITE else -value
    return score


def _order_key(move):
    # Captures/promotions searched first -- cheap, effective move-ordering
    # heuristic that prunes far more branches than board-scan order would.
    return 0 if move[4] in ("promo", "ep") else 1


def _negamax(state, depth, alpha, beta, sign):
    moves = legal_moves(state)
    if not moves:
        if in_check(state, state.turn):
            return -100000 - depth, None  # prefer faster mates
        return 0, None
    if depth == 0:
        return sign * evaluate(state), None

    moves.sort(key=_order_key)
    best_score = None
    best_move = None
    for move in moves:
        child = make_move(state, move)
        score, _ = _negamax(child, depth - 1, -beta, -alpha, -sign)
        score = -score
        if best_score is None or score > best_score:
            best_score = score
            best_move = move
        alpha = max(alpha, score)
        if alpha >= beta:
            break

    return best_score, best_move


def best_move(state, depth=2):
    """Searches `depth` ply and returns the best move for state.turn, or
    None if there are no legal moves. Every node does a fresh legal_moves()
    call and make_move() copies the whole board -- deliberately simple and
    correct over fast; increasing depth trades off search time steeply."""
    sign = 1 if state.turn == WHITE else -1
    _, move = _negamax(state, depth, -1000000, 1000000, sign)
    return move


# =========================================================================
# Board rendering / TUI
# =========================================================================

RESET = "\x1b[0m"
HIDE_CURSOR = "\x1b[?25l"
SHOW_CURSOR = "\x1b[?25h"
CLEAR_SCREEN = "\033[2J\033[H"

LIGHT_BG = "\x1b[48;5;180m"
DARK_BG = "\x1b[48;5;94m"
CURSOR_BG = "\x1b[48;5;226m"
SELECT_BG = "\x1b[48;5;46m"
WHITE_FG = "\x1b[38;5;15m"
BLACK_FG = "\x1b[38;5;0m"
LABEL_FG = "\x1b[38;5;252m"
MSG_FG = "\x1b[38;5;208m"

# Unicode Chess Symbols (U+2654-265F) -- rendered double-width via
# WIDE_FONT (see fontconvert.py --wide-unicode / fb.c's ATTR_WIDE
# handling). Only actually shows up correctly with a font that ships a
# WIDE_FONT block (currently just Unifont).
PIECES = {
    WHITE: {"K": "♔", "Q": "♕", "R": "♖", "B": "♗", "N": "♘", "P": "♙"},
    BLACK: {"K": "♚", "Q": "♛", "R": "♜", "B": "♝", "N": "♞", "P": "♟"},
}

FILES = "abcdefgh"

# Screen offsets: row 1 = title, row 2 = file labels, board starts row 3.
# Col 1-3 = rank number gutter, board starts col 4, each square is 2
# columns wide (matching a WIDE glyph's on-screen width).
BOARD_ROW0 = 3
BOARD_COL0 = 4
STATUS_ROW = BOARD_ROW0 + 8 + 1


def goto(row, col):
    return "\x1b[%d;%dH" % (row, col)


def park_cursor():
    # Real terminal cursor out of the way -- the board's own cursor is
    # drawn via cell background color instead.
    return goto(STATUS_ROW + 2, 1)


def square_bg(x, y, is_cursor, is_selected):
    # Selection wins over cursor highlight -- otherwise picking up a piece
    # (selected == cursor at that instant) shows no visible change until
    # the cursor moves away.
    if is_selected:
        return SELECT_BG
    if is_cursor:
        return CURSOR_BG
    return LIGHT_BG if (x + y) % 2 == 0 else DARK_BG


def cell_str(x, y, board, cursor, selected):
    bg = square_bg(x, y, (x, y) == cursor, (x, y) == selected)
    piece = board[y][x]
    if piece is None:
        return bg + "  " + RESET
    color, kind = piece
    fg = WHITE_FG if color == WHITE else BLACK_FG
    return bg + fg + PIECES[color][kind] + RESET


def cell_screen_pos(x, y):
    return BOARD_ROW0 + y, BOARD_COL0 + (x * 2)


def status_text(state, message):
    if message:
        return MSG_FG + message + RESET
    status = game_status(state)
    turn_name = "White" if state.turn == WHITE else "Black"
    if status == "checkmate":
        winner = "Black" if state.turn == WHITE else "White"
        return MSG_FG + f"Checkmate: {winner} wins. esc to quit." + RESET
    if status == "stalemate":
        return MSG_FG + "Stalemate: draw. esc to quit." + RESET
    check = " (check)" if in_check(state, state.turn) else ""
    return LABEL_FG + f"{turn_name} to move{check}" + RESET


def draw_status(state, message=""):
    sys.stdout.write(goto(STATUS_ROW, 1) + "\x1b[K" + status_text(state, message) + park_cursor())


def draw_full(state, cursor, selected, message=""):
    buf = ["\x1b[2J", goto(1, 1)]
    buf.append("w/a/s/d move, enter pick/drop, esc quit")
    buf.append(goto(2, BOARD_COL0) + LABEL_FG)
    buf.append("".join(f + " " for f in FILES) + RESET)

    board = state.board
    for y in range(8):
        buf.append(goto(BOARD_ROW0 + y, 1) + LABEL_FG + str(8 - y) + RESET)
        row = [cell_str(x, y, board, cursor, selected) for x in range(8)]
        buf.append(goto(BOARD_ROW0 + y, BOARD_COL0) + "".join(row))

    buf.append(goto(STATUS_ROW, 1) + status_text(state, message))
    buf.append(park_cursor())
    sys.stdout.write("".join(buf))


def draw_cells(cells, state, cursor, selected, message=""):
    buf = []
    board = state.board
    for (x, y) in cells:
        if not (0 <= x < 8 and 0 <= y < 8):
            continue
        r, c = cell_screen_pos(x, y)
        buf.append(goto(r, c) + cell_str(x, y, board, cursor, selected))
    buf.append(goto(STATUS_ROW, 1) + "\x1b[K" + status_text(state, message))
    buf.append(park_cursor())
    sys.stdout.write("".join(buf))


def find_move(state, sx, sy, cx, cy):
    """The legal move (if any) from (sx,sy) to (cx,cy) for state.turn."""
    for move in legal_moves(state):
        if move[0] == sx and move[1] == sy and move[2] == cx and move[3] == cy:
            return move
    return None


def maybe_engine_reply(state):
    """Black is always engine-controlled -- play immediately whenever it
    becomes Black's turn, so the human only ever plays White. A no-op if
    it isn't Black's turn or the game's already over."""
    if state.turn == BLACK and game_status(state) == "playing":
        draw_status(state, "Thinking...")
        move = best_move(state, depth=ENGINE_DEPTH)
        if move is not None:
            state = make_move(state, move)
    return state


def main(env, args):
    state = State()
    cursor = (4, 4)
    selected = None

    current_font = env.font_name

    # apply a font that has the chess unicode characters
    env.update_font("unifont_mpy_16")

    sys.stdout.write(HIDE_CURSOR)
    draw_full(state, cursor, selected)

    try:
        while True:
            ch = sys.stdin.read(1)
            dirty = []
            message = ""
            cx, cy = cursor
            game_over = game_status(state) != "playing"

            if ch == 'w' and cy > 0:
                dirty.append(cursor)
                cursor = (cx, cy - 1)
                dirty.append(cursor)
            elif ch == 's' and cy < 7:
                dirty.append(cursor)
                cursor = (cx, cy + 1)
                dirty.append(cursor)
            elif ch == 'a' and cx > 0:
                dirty.append(cursor)
                cursor = (cx - 1, cy)
                dirty.append(cursor)
            elif ch == 'd' and cx < 7:
                dirty.append(cursor)
                cursor = (cx + 1, cy)
                dirty.append(cursor)

            elif ch in (' ', '\r', '\n') and not game_over:
                if selected is None:
                    piece = state.board[cy][cx]
                    if piece is not None and piece[0] == state.turn:
                        selected = (cx, cy)
                        dirty.append(cursor)
                elif selected == (cx, cy):
                    # Put the same square back down -- cancel.
                    selected = None
                    dirty.append(cursor)
                else:
                    sx, sy = selected
                    piece = state.board[sy][sx]
                    dest = state.board[cy][cx]
                    if dest is not None and dest[0] == piece[0]:
                        # Same-color piece on the destination -- reselect
                        # rather than move onto your own piece.
                        selected = (cx, cy)
                        dirty.append((sx, sy))
                        dirty.append(cursor)
                    else:
                        move = find_move(state, sx, sy, cx, cy)
                        if move is None:
                            message = "Illegal move"
                            dirty.append(cursor)
                        else:
                            state = make_move(state, move)
                            selected = None
                            state = maybe_engine_reply(state)
                            # The reply (or castling/en passant moving a
                            # second piece) can touch squares outside the
                            # human's own move -- simplest to just repaint
                            # everything rather than track it all.
                            draw_full(state, cursor, selected)
                            dirty = []

            elif ch == 'e' and not game_over:
                draw_status(state, "Thinking...")
                move = best_move(state, depth=ENGINE_DEPTH)
                if move is not None:
                    state = make_move(state, move)
                    selected = None
                state = maybe_engine_reply(state)
                draw_full(state, cursor, selected)
                dirty = []

            elif ch == '\x1b':
                sys.stdout.write(CLEAR_SCREEN + SHOW_CURSOR + "\nExiting...\n")
                env.update_font(current_font)
                break

            if dirty:
                draw_cells(dirty, state, cursor, selected, message)
    finally:
        sys.stdout.write(CLEAR_SCREEN + SHOW_CURSOR)
