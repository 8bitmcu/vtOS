/*
 * MicroPython Text User Interface Library
 * Copyright (c) 2026 8bitmcu
 * License: MIT
 */

#include "py/mpconfig.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "py/runtime.h"
#include <string.h>

// ─── Stream globals
// ───────────────────────────────────────────────────────────

// stream_obj is kept only for draw() and repaint_bars() calls.
// All ANSI output goes through mp_hal_stdout_tx_strn so it reaches both
// the USB serial REPL and the LCD (via dupterm → KVM → term).
static mp_obj_t vttui_stream_obj = MP_OBJ_NULL;

static void vttui_write(const char *buf, size_t len) {
  if (vttui_stream_obj == MP_OBJ_NULL || len == 0)
    return;
  mp_hal_stdout_tx_strn(buf, len);
}

static void vttui_write_str(const char *str) { vttui_write(str, strlen(str)); }

// ─── Escape sequence helpers
// ──────────────────────────────────────────────────

static int write_uint(char *buf, unsigned int n) {
  if (n == 0) {
    buf[0] = '0';
    return 1;
  }
  char tmp[10];
  int len = 0;
  while (n > 0) {
    tmp[len++] = '0' + (n % 10);
    n /= 10;
  }
  for (int i = 0; i < len; i++)
    buf[i] = tmp[len - 1 - i];
  return len;
}

// ESC[row+1;col+1H  ESC[0;{1;}38;5;fg;48;5;bgm
static void vttui_begin(int col, int row, uint32_t fg, uint32_t bg, bool bold) {
  // fg/bg/row/col are meant to be small (ANSI-256 index, screen
  // coordinate) but nothing clamps them before they get here -- write_uint()
  // emits up to 10 digits for a full uint32_t, and the worst case (two
  // 10-digit coordinates + two 10-digit colors) is 62 bytes, so this needs
  // real headroom rather than a size tuned to the expected/common case.
  char buf[80];
  int i = 0;
  buf[i++] = '\033';
  buf[i++] = '[';
  i += write_uint(buf + i, (unsigned)row + 1);
  buf[i++] = ';';
  i += write_uint(buf + i, (unsigned)col + 1);
  buf[i++] = 'H';
  buf[i++] = '\033';
  buf[i++] = '[';
  buf[i++] = '0';
  if (bold) {
    buf[i++] = ';';
    buf[i++] = '1';
  }
  buf[i++] = ';';
  buf[i++] = '3';
  buf[i++] = '8';
  buf[i++] = ';';
  buf[i++] = '5';
  buf[i++] = ';';
  i += write_uint(buf + i, fg);
  buf[i++] = ';';
  buf[i++] = '4';
  buf[i++] = '8';
  buf[i++] = ';';
  buf[i++] = '5';
  buf[i++] = ';';
  i += write_uint(buf + i, bg);
  buf[i++] = 'm';
  vttui_write(buf, i);
}

// ESC[0;{1;}38;5;fg;48;5;bgm  (color only, no cursor move)
static void vttui_sgr(uint32_t fg, uint32_t bg, bool bold) {
  // See the same-sized-concern comment in vttui_begin() above -- worst
  // case (two 10-digit colors) is 38 bytes, not the ~24 the expected
  // 3-digit-color case needs.
  char buf[48];
  int i = 0;
  buf[i++] = '\033';
  buf[i++] = '[';
  buf[i++] = '0';
  if (bold) {
    buf[i++] = ';';
    buf[i++] = '1';
  }
  buf[i++] = ';';
  buf[i++] = '3';
  buf[i++] = '8';
  buf[i++] = ';';
  buf[i++] = '5';
  buf[i++] = ';';
  i += write_uint(buf + i, fg);
  buf[i++] = ';';
  buf[i++] = '4';
  buf[i++] = '8';
  buf[i++] = ';';
  buf[i++] = '5';
  buf[i++] = ';';
  i += write_uint(buf + i, bg);
  buf[i++] = 'm';
  vttui_write(buf, i);
}

// ESC[row+1;col+1H  (position only, no attribute change)
static void vttui_move(int col, int row) {
  // Same headroom concern as vttui_begin()/vttui_sgr(): a negative row/col
  // (e.g. from a miscalculated scroll position) becomes a huge value once
  // cast to unsigned, and write_uint() can emit up to 10 digits for each
  // -- worst case is 24 bytes, not the ~8 the expected small-coordinate
  // case needs.
  char buf[32];
  int i = 0;
  buf[i++] = '\033';
  buf[i++] = '[';
  i += write_uint(buf + i, (unsigned)row + 1);
  buf[i++] = ';';
  i += write_uint(buf + i, (unsigned)col + 1);
  buf[i++] = 'H';
  vttui_write(buf, i);
}

static void write_spaces(int n) {
  static const char sp32[] = "                                ";
  while (n >= 32) {
    vttui_write(sp32, 32);
    n -= 32;
  }
  if (n > 0)
    vttui_write(sp32, n);
}

static void write_repeat_str(const char *s, size_t slen, int n) {
  for (int i = 0; i < n; i++)
    vttui_write(s, slen);
}

// Forward declarations: full definitions live in the VTBlock section below,
// but VTList's wrap support (render_list_item_multiline / list_item_rows)
// needs them earlier in the file.
static int next_chunk_len(const char *s, int byte_len, int width, int *out_vis);
static int line_display_rows(const char *s, int len, bool wrap, int width);
// write_block_text re-injects fg/bg after any \033[0m / \033[m embedded in
// the text -- render_label_raw and render_list_item need that same
// reset-reinjection so a color reset mid-item doesn't leak the terminal's
// true default background into the padding/rows drawn after it.
static void write_block_text(const char *text, int len, uint32_t fg, uint32_t bg);
// replay_ansi_state re-emits any escape sequences found in [start, end) --
// render_list_item needs this for wrapped continuation rows, so a color
// opened earlier in the same logical line (but not yet closed) carries
// over onto the next screen row instead of reverting to fg/bg.
static void replay_ansi_state(const char *start, const char *end, uint32_t fg,
                              uint32_t bg);

// ─── UTF-8 box drawing
// ────────────────────────────────────────────────────────

#define BOX_TL "\xe2\x94\x8c" // ┌
#define BOX_TR "\xe2\x94\x90" // ┐
#define BOX_BL "\xe2\x94\x94" // └
#define BOX_BR "\xe2\x94\x98" // ┘
#define BOX_H "\xe2\x94\x80"  // ─
#define BOX_V "\xe2\x94\x82"  // │

// ─── Internal render helpers
// ──────────────────────────────────────────────────

static void render_label_raw(int start_x, int y, const char *text,
                             size_t text_len, int draw_width, uint32_t fg,
                             uint32_t bg, bool bold, int text_offset) {
  if (draw_width <= 0)
    return;
  vttui_begin(start_x, y, fg, bg, bold);

  // Split draw_width into: prefix spaces | text | suffix spaces
  int prefix = text_offset;
  int len = (int)text_len;
  if (prefix < 0) {
    text -= prefix;
    len += prefix;
    prefix = 0;
  }
  if (len < 0)
    len = 0;

  // text may contain embedded ANSI escapes (e.g. a caller coloring part of
  // the label), which take up bytes but zero screen columns -- clamping by
  // raw byte length against draw_width (as this used to) either truncates
  // mid-escape or, for short colored text, wrongly believes there's no
  // room left for the suffix. next_chunk_len() bounds by *visible* width
  // instead, atomically including/excluding whole escape sequences, and
  // out_vis reports how many actual columns that consumed.
  int budget = draw_width - prefix;
  int vis = 0;
  int chars = budget > 0 ? next_chunk_len(text, len, budget, &vis) : 0;
  int suffix = draw_width - prefix - vis;
  if (suffix < 0)
    suffix = 0;

  write_spaces(prefix);
  if (chars > 0)
    write_block_text(text, chars, fg, bg);
  write_spaces(suffix);

  vttui_write_str("\033[0m");
}

// Arrow is stored as raw UTF-8 bytes (up to 4) so multi-byte chars work.
//
// prior/prior_len are the bytes of the same logical line that came before
// `text` (i.e. earlier wrapped chunks already rendered on previous screen
// rows), or NULL/0 if `text` is the start of the line. When non-empty,
// any still-open color from those earlier chunks gets replayed onto this
// row before the text itself -- without it, a color opened on an earlier
// row wouldn't carry over, since vttui_begin() below always starts each
// row fresh at the base fg/bg.
static void render_list_item(int x, int y, const char *text, size_t text_len,
                             int width, uint32_t fg, uint32_t bg,
                             const uint8_t *arrow_bytes, int arrow_len,
                             bool show_arrow, int left_pad,
                             const char *prior, size_t prior_len) {
  if (width <= 0)
    return;
  vttui_begin(x, y, fg, bg, false);

  int remaining = width;

  if (arrow_len > 0) {
    if (show_arrow)
      vttui_write((const char *)arrow_bytes, (size_t)arrow_len);
    else
      vttui_write(" ", 1);
    remaining--;
  }

  int pad = left_pad < remaining ? left_pad : remaining;
  write_spaces(pad);
  remaining -= pad;

  if (prior_len > 0)
    replay_ansi_state(prior, prior + prior_len, fg, bg);

  // text may contain embedded ANSI escapes (e.g. gemtext link/heading
  // coloring) -- see the matching comment in render_label_raw for why
  // clamping by raw byte length against `remaining` (a column count) is
  // wrong, and why next_chunk_len()/out_vis is the fix.
  int vis = 0;
  int chars = remaining > 0 ? next_chunk_len(text, (int)text_len, remaining, &vis) : 0;
  if (chars > 0)
    write_block_text(text, chars, fg, bg);
  remaining -= vis;
  if (remaining < 0)
    remaining = 0;

  write_spaces(remaining);
  vttui_write_str("\033[0m");
}

// Renders a (possibly multi-line, '\n'-separated, and/or word/char-wrapped)
// item starting at row y, using up to max_rows screen rows. The arrow (if
// any) is only drawn on the item's very first row; every other row gets the
// same fg/bg but no arrow glyph, just left_pad + text, so multi-row items
// still look like one highlighted block. Returns the number of rows
// actually drawn (<= max_rows).
//
// When do_wrap is true, each '\n'-delimited line is additionally split into
// chunks of at most text_width visible characters (via next_chunk_len,
// which is UTF-8 and ANSI-escape aware); when false, each '\n'-delimited
// line is rendered as a single row (clipped to `width` by render_list_item,
// same as before wrap support existed).
static int render_list_item_multiline(int x, int y, const char *text,
                                      size_t text_len, int width, int max_rows,
                                      uint32_t fg, uint32_t bg,
                                      const uint8_t *arrow_bytes, int arrow_len,
                                      bool show_arrow, int left_pad,
                                      bool do_wrap, int text_width) {
  const char *p = text;
  const char *end = text + text_len;
  int row = 0;
  bool first_line = true;
  while (row < max_rows) {
    const char *line_start = p;
    while (p < end && *p != '\n')
      p++;
    size_t line_len = (size_t)(p - line_start);
    bool at_newline = (p < end);

    if (do_wrap && text_width > 0) {
      int remaining = (int)line_len;
      const char *pos = line_start;
      bool first_chunk = true;
      while ((first_chunk || remaining > 0) && row < max_rows) {
        int cl =
            remaining > 0 ? next_chunk_len(pos, remaining, text_width, NULL) : 0;
        bool arrow_this_row = show_arrow && first_line && first_chunk;
        render_list_item(x, y + row, pos, (size_t)cl, width, fg, bg,
                         arrow_bytes, arrow_len, arrow_this_row, left_pad,
                         line_start, (size_t)(pos - line_start));
        row++;
        if (cl == 0)
          break;
        pos += cl;
        remaining -= cl;
        first_chunk = false;
      }
    } else {
      bool arrow_this_row = show_arrow && first_line;
      render_list_item(x, y + row, line_start, line_len, width, fg, bg,
                       arrow_bytes, arrow_len, arrow_this_row, left_pad,
                       NULL, 0);
      row++;
    }

    first_line = false;
    if (at_newline) {
      p++;
    } else {
      break;
    }
  }
  return row;
}

// ─── Object structs
// ───────────────────────────────────────────────────────────

typedef struct _vttui_vttui_obj_t {
  mp_obj_base_t base;
  mp_obj_t stream_obj;
  uint16_t width;
  uint16_t height;
} vttui_vttui_obj_t;

typedef struct _vttui_label_obj_t {
  mp_obj_base_t base;
  mp_obj_t text_obj; // Python string, kept alive for GC
  int abs_x, abs_y;
  int draw_width;
  uint32_t fg, bg;
  bool bold;
  int text_offset;
  bool dirty;
} vttui_label_obj_t;

typedef struct _vttui_list_obj_t {
  mp_obj_base_t base;
  mp_obj_t items;
  int item_count;
  int x, y;
  int width, height;
  uint32_t fg, bg;
  uint32_t sel_fg, sel_bg;
  int selected, scroll;
  int last_selected, last_scroll;
  mp_obj_t on_change;
  uint8_t arrow[4]; // UTF-8 bytes of arrow char
  int arrow_len;    // 0 = disabled
  int left_pad;
  mp_obj_t title;
  bool decorations;
  bool multiline; // items may contain '\n' and span multiple rows
  bool wrap;      // auto-wrap item text to the list's item width
} vttui_list_obj_t;

typedef struct _vttui_window_obj_t {
  mp_obj_base_t base;
  int x, y, width, height;
  int inner_x, inner_y, inner_w, inner_h;
  uint32_t fg, bg;
  mp_obj_t title;
  bool decorations;
  bool dirty;
} vttui_window_obj_t;

#define VTTUI_INPUT_MAX 255

typedef struct _vttui_input_obj_t {
  mp_obj_base_t base;
  int abs_x, abs_y;
  int width; // total box width including borders
  uint32_t fg, bg;
  bool bold, secret;
  mp_obj_t label_obj;
  uint32_t input_bg;
  char buf[VTTUI_INPUT_MAX + 1];
  int buf_len;
  bool decorations;
} vttui_input_obj_t;

typedef struct _vttui_block_obj_t {
  mp_obj_base_t base;
  int abs_x, abs_y;
  int width;  // 0 = no fill; >0 = clear each line to this width before writing
  int height; // 0 = unlimited; >0 = visible rows (used for scroll)
  bool scroll;
  bool wrap;
  uint32_t fg, bg; // baseline color; inherited from parent or explicitly set
  mp_obj_t text_obj;
  bool dirty;
} vttui_block_obj_t;

typedef struct _vttui_dialog_obj_t {
  mp_obj_base_t base;
  int x, y, width, height;
  uint32_t fg, bg;
  uint32_t sel_fg, sel_bg;
  mp_obj_t text;
  mp_obj_t btn1, btn2;
  mp_obj_t title;
  int selected;
  int last_selected; // -1 = never drawn (triggers full redraw)
  bool decorations;
} vttui_dialog_obj_t;

// ─── VTLabel
// ──────────────────────────────────────────────────────────────────

static mp_obj_t vttui_label_draw(mp_obj_t self_in) {
  vttui_label_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (!self->dirty)
    return mp_const_none;
  size_t text_len;
  const char *text = mp_obj_str_get_data(self->text_obj, &text_len);
  render_label_raw(self->abs_x, self->abs_y, text, text_len, self->draw_width,
                   self->fg, self->bg, self->bold, self->text_offset);
  self->dirty = false;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_label_draw_obj, vttui_label_draw);

static mp_obj_t vttui_label_invalidate(mp_obj_t self_in) {
  ((vttui_label_obj_t *)MP_OBJ_TO_PTR(self_in))->dirty = true;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_label_invalidate_obj,
                                 vttui_label_invalidate);

static const mp_rom_map_elem_t vttui_label_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_draw), MP_ROM_PTR(&vttui_label_draw_obj)},
    {MP_ROM_QSTR(MP_QSTR_invalidate), MP_ROM_PTR(&vttui_label_invalidate_obj)},
};
static MP_DEFINE_CONST_DICT(vttui_label_locals_dict,
                            vttui_label_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(vttui_label_type, MP_QSTR_VTLabel, MP_TYPE_FLAG_NONE,
                         locals_dict, &vttui_label_locals_dict);

// Shared arg table for make_label / draw_label on both VTTUI and VTWindow.
static const mp_arg_t make_label_args[] = {
    {MP_QSTR_text, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
    {MP_QSTR_x, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_y, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_fg, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1}},
    {MP_QSTR_bg, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1}},
    {MP_QSTR_bold, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false}},
    {MP_QSTR_align, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none}},
    {MP_QSTR_width, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0}},
};

static int parse_align(mp_obj_t obj) {
  if (obj == mp_const_none)
    return 0;
  size_t len;
  const char *s = mp_obj_str_get_data(obj, &len);
  if (len == 6 && memcmp(s, "center", 6) == 0)
    return 1;
  if (len == 5 && memcmp(s, "right", 5) == 0)
    return 2;
  return 0;
}

// Allocate a VTLabel with pre-computed layout.
// origin_x/y: absolute top-left of the container; container_w: used for
// centering. align: 0=left, 1=center, 2=right
static vttui_label_obj_t *create_label(mp_obj_t text_obj, int rel_x, int rel_y,
                                       int origin_x, int origin_y,
                                       int container_w, uint32_t fg,
                                       uint32_t bg, bool bold, int req_width,
                                       int align) {
  size_t text_len;
  mp_obj_str_get_data(text_obj, &text_len);

  int draw_width = req_width > 0 ? req_width : (int)text_len;

  int start_x = (align == 1) ? origin_x + (container_w - draw_width) / 2
                             : origin_x + rel_x;
  if (start_x < origin_x)
    start_x = origin_x;

  int text_offset;
  if (align == 1)
    text_offset = (draw_width - (int)text_len) / 2;
  else if (align == 2)
    text_offset = draw_width - (int)text_len;
  else
    text_offset = 0;
  if (text_offset < 0)
    text_offset = 0;

  vttui_label_obj_t *lbl = m_new_obj(vttui_label_obj_t);
  lbl->base.type = &vttui_label_type;
  lbl->text_obj = text_obj;
  lbl->abs_x = start_x;
  lbl->abs_y = origin_y + rel_y;
  lbl->draw_width = draw_width;
  lbl->fg = fg;
  lbl->bg = bg;
  lbl->bold = bold;
  lbl->text_offset = text_offset;
  lbl->dirty = true;
  return lbl;
}

// ─── VTList
// ───────────────────────────────────────────────────────────────────

static bool list_is_multirow(vttui_list_obj_t *self);
static int list_text_width(vttui_list_obj_t *self);

static mp_obj_t vttui_list_draw(mp_obj_t self_in) {
  vttui_list_obj_t *self = MP_OBJ_TO_PTR(self_in);
  bool first_draw = (self->last_scroll == -1);
  bool scroll_changed = (self->scroll != self->last_scroll);

  int item_x, item_y, item_w, vis_h, item_row_offset;

  if (self->decorations) {
    item_x = self->x + 1;
    item_y = self->y + 1;
    item_w = self->width - 2;
    vis_h = self->height - 2;
    item_row_offset = 0;

    if (first_draw) {
      int x = self->x, y = self->y, w = self->width, h = self->height;
      vttui_begin(x, y, self->fg, self->bg, false);
      vttui_write_str(BOX_TL);
      if (self->title != mp_const_none) {
        size_t tlen;
        const char *title = mp_obj_str_get_data(self->title, &tlen);
        int inner = w - 2, label_w = (int)tlen + 2;
        if (label_w <= inner) {
          int n1 = (inner - label_w) / 2;
          int n2 = inner - label_w - n1;
          write_repeat_str(BOX_H, 3, n1);
          vttui_write_str("\033[1m");
          vttui_write(" ", 1);
          vttui_write(title, tlen);
          vttui_write(" ", 1);
          vttui_write_str("\033[0m");
          vttui_begin(x + 1 + n1 + label_w, y, self->fg, self->bg, false);
          write_repeat_str(BOX_H, 3, n2);
        } else {
          write_repeat_str(BOX_H, 3, inner);
        }
      } else {
        write_repeat_str(BOX_H, 3, w - 2);
      }
      vttui_write_str(BOX_TR "\033[0m");
      for (int row = 1; row < h - 1; row++) {
        vttui_begin(x, y + row, self->fg, self->bg, false);
        vttui_write_str(BOX_V "\033[0m");
        vttui_begin(x + w - 1, y + row, self->fg, self->bg, false);
        vttui_write_str(BOX_V "\033[0m");
      }
      vttui_begin(x, y + h - 1, self->fg, self->bg, false);
      vttui_write_str(BOX_BL);
      write_repeat_str(BOX_H, 3, w - 2);
      vttui_write_str(BOX_BR "\033[0m");
    }
  } else {
    item_x = self->x;
    item_y = self->y;
    item_w = self->width;
    vis_h = self->height;
    item_row_offset = (self->title != mp_const_none) ? 1 : 0;

    if (first_draw && self->title != mp_const_none) {
      size_t tlen;
      const char *title = mp_obj_str_get_data(self->title, &tlen);
      render_label_raw(self->x, self->y, title, tlen, self->width, self->fg,
                       self->bg, false, 0);
    }
  }

  if (list_is_multirow(self)) {
    // Items can span several rows, so a per-row is_sel/was_sel diff (as used
    // below for the single-row case) doesn't work. Instead, just redraw all
    // visible items whenever something that affects them has changed.
    if (first_draw || scroll_changed || self->selected != self->last_selected) {
      int text_width = self->wrap ? list_text_width(self) : 0;
      int row = 0, item_idx = self->scroll;
      while (row < vis_h && item_idx < self->item_count) {
        bool is_sel = (item_idx == self->selected);
        uint32_t fg = is_sel ? self->sel_fg : self->fg;
        uint32_t bg = is_sel ? self->sel_bg : self->bg;

        mp_obj_t item = mp_obj_subscr(
            self->items, MP_OBJ_NEW_SMALL_INT(item_idx), MP_OBJ_SENTINEL);
        size_t text_len;
        const char *text = mp_obj_str_get_data(item, &text_len);
        int used = render_list_item_multiline(
            item_x, item_y + item_row_offset + row, text, text_len, item_w,
            vis_h - row, fg, bg, self->arrow, self->arrow_len, is_sel,
            self->left_pad, self->wrap, text_width);
        row += used;
        item_idx++;
      }
      // Clear any leftover rows below the last item.
      while (row < vis_h) {
        render_list_item(item_x, item_y + item_row_offset + row, "", 0, item_w,
                         self->fg, self->bg, self->arrow, self->arrow_len,
                         false, self->left_pad, NULL, 0);
        row++;
      }
    }
  } else {
    for (int i = 0; i < vis_h; i++) {
      int item_idx = self->scroll + i;
      int row = item_y + item_row_offset + i;
      bool is_sel = (item_idx == self->selected);
      bool was_sel = (item_idx == self->last_selected);

      if (!scroll_changed && (is_sel == was_sel))
        continue;

      uint32_t fg = is_sel ? self->sel_fg : self->fg;
      uint32_t bg = is_sel ? self->sel_bg : self->bg;

      if (item_idx >= self->item_count) {
        render_list_item(item_x, row, "", 0, item_w, fg, bg, self->arrow,
                         self->arrow_len, false, self->left_pad, NULL, 0);
        continue;
      }

      mp_obj_t item = mp_obj_subscr(self->items, MP_OBJ_NEW_SMALL_INT(item_idx),
                                    MP_OBJ_SENTINEL);
      size_t text_len;
      const char *text = mp_obj_str_get_data(item, &text_len);
      render_list_item(item_x, row, text, text_len, item_w, fg, bg, self->arrow,
                       self->arrow_len, is_sel, self->left_pad, NULL, 0);
    }
  }

  self->last_selected = self->selected;
  self->last_scroll = self->scroll;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_list_draw_obj, vttui_list_draw);

// Whether this list uses multi-row rendering at all (explicit '\n' splits,
// auto-wrap, or both).
static bool list_is_multirow(vttui_list_obj_t *self) {
  return self->multiline || self->wrap;
}

// Visible-character width available for item text, i.e. item width minus
// the arrow column and left_pad — mirrors the math render_list_item() does
// internally, so wrap boundaries line up with what's actually drawn.
static int list_text_width(vttui_list_obj_t *self) {
  int item_w = self->decorations ? self->width - 2 : self->width;
  int w = item_w;
  if (self->arrow_len > 0)
    w--;
  w -= self->left_pad;
  if (w < 1)
    w = 1;
  return w;
}

// Number of screen rows item `idx` occupies: 1 unless multiline/wrap is
// enabled, in which case it's the sum of wrapped-row counts across each
// '\n'-delimited line (wrap-splitting only happens if self->wrap is set).
static int list_item_rows(vttui_list_obj_t *self, int idx) {
  if (!list_is_multirow(self))
    return 1;
  mp_obj_t item =
      mp_obj_subscr(self->items, MP_OBJ_NEW_SMALL_INT(idx), MP_OBJ_SENTINEL);
  size_t len;
  const char *text = mp_obj_str_get_data(item, &len);
  int text_width = self->wrap ? list_text_width(self) : 0;

  int rows = 0;
  const char *ls = text;
  const char *end = text + len;
  for (const char *p = text; p <= end; p++) {
    if (p == end || *p == '\n') {
      if (p == end && p == ls)
        break; // skip trailing empty segment after a final '\n'
      int line_len = (int)(p - ls);
      rows +=
          self->wrap ? line_display_rows(ls, line_len, true, text_width) : 1;
      ls = p + 1;
    }
  }
  return rows > 0 ? rows : 1;
}

static void list_set_selected(vttui_list_obj_t *self, int idx) {
  if (idx < 0)
    idx = 0;
  if (idx >= self->item_count)
    idx = self->item_count - 1;
  self->selected = idx;
  int vis_h = self->decorations ? self->height - 2 : self->height;

  if (!list_is_multirow(self)) {
    if (self->selected < self->scroll)
      self->scroll = self->selected;
    else if (self->selected >= self->scroll + vis_h)
      self->scroll = self->selected - vis_h + 1;
  } else {
    // scroll is an item index (not a row count); walk it so that the run of
    // items [scroll, selected] fits within vis_h screen rows.
    if (self->selected < self->scroll) {
      self->scroll = self->selected;
    } else {
      while (self->scroll < self->selected) {
        int rows = 0;
        for (int i = self->scroll; i <= self->selected; i++)
          rows += list_item_rows(self, i);
        if (rows <= vis_h)
          break;
        self->scroll++;
      }
    }
  }
  if (self->on_change != mp_const_none)
    mp_call_function_1(self->on_change, MP_OBJ_NEW_SMALL_INT(self->selected));
}

static mp_obj_t vttui_list_up(mp_obj_t self_in) {
  vttui_list_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (self->selected > 0)
    list_set_selected(self, self->selected - 1);
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_list_up_obj, vttui_list_up);

static mp_obj_t vttui_list_down(mp_obj_t self_in) {
  vttui_list_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (self->selected < self->item_count - 1)
    list_set_selected(self, self->selected + 1);
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_list_down_obj, vttui_list_down);

static mp_obj_t vttui_list_select(mp_obj_t self_in, mp_obj_t idx_obj) {
  list_set_selected(MP_OBJ_TO_PTR(self_in), mp_obj_get_int(idx_obj));
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(vttui_list_select_obj, vttui_list_select);

static const mp_rom_map_elem_t vttui_list_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_draw), MP_ROM_PTR(&vttui_list_draw_obj)},
    {MP_ROM_QSTR(MP_QSTR_up), MP_ROM_PTR(&vttui_list_up_obj)},
    {MP_ROM_QSTR(MP_QSTR_down), MP_ROM_PTR(&vttui_list_down_obj)},
    {MP_ROM_QSTR(MP_QSTR_select), MP_ROM_PTR(&vttui_list_select_obj)},
};
static MP_DEFINE_CONST_DICT(vttui_list_locals_dict,
                            vttui_list_locals_dict_table);

static void vttui_list_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
  vttui_list_obj_t *self = MP_OBJ_TO_PTR(self_in);

  if (dest[0] == MP_OBJ_NULL) {
    // Load.
    if (attr == MP_QSTR_selected || attr == MP_QSTR_index) {
      dest[0] = MP_OBJ_NEW_SMALL_INT(self->selected);
      return;
    }
    if (attr == MP_QSTR_value) {
      dest[0] = mp_obj_subscr(self->items, MP_OBJ_NEW_SMALL_INT(self->selected),
                              MP_OBJ_SENTINEL);
      return;
    }
    mp_map_elem_t *elem = mp_map_lookup((mp_map_t *)&vttui_list_locals_dict.map,
                                        MP_OBJ_NEW_QSTR(attr), MP_MAP_LOOKUP);
    if (elem) {
      dest[0] = elem->value;
      dest[1] = self_in;
    }
    return;
  }

  if (dest[0] == MP_OBJ_SENTINEL) {
    // Store. `index` is the only writable attribute: setting it moves the
    // selection (and adjusts scroll, and fires on_change) exactly like
    // calling select(idx).
    if (attr == MP_QSTR_index) {
      list_set_selected(self, mp_obj_get_int(dest[1]));
      dest[0] = MP_OBJ_NULL; // signal success
    }
    return;
  }
}

MP_DEFINE_CONST_OBJ_TYPE(vttui_list_type, MP_QSTR_VTList, MP_TYPE_FLAG_NONE,
                         attr, vttui_list_attr, locals_dict,
                         &vttui_list_locals_dict);

// Shared list init used by both vttui_make_list and vttui_window_make_list.
static const mp_arg_t make_list_args[] = {
    {MP_QSTR_items, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
    {MP_QSTR_x, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_y, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_width, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_height, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_fg, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_bg, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_sel_fg, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1}},
    {MP_QSTR_sel_bg, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1}},
    {MP_QSTR_selected, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_on_change, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none}},
    {MP_QSTR_arrow, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none}},
    {MP_QSTR_left_pad, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_align, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none}},
    {MP_QSTR_title, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none}},
    {MP_QSTR_decorations, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false}},
    {MP_QSTR_multiline, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false}},
    {MP_QSTR_wrap, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false}},
};

static vttui_list_obj_t *create_list(mp_arg_val_t *args, int abs_x, int abs_y,
                                     int width, int height) {
  vttui_list_obj_t *list = m_new_obj(vttui_list_obj_t);
  list->base.type = &vttui_list_type;
  list->items = args[0].u_obj;
  list->item_count = mp_obj_get_int(mp_obj_len(list->items));
  list->x = abs_x;
  list->y = abs_y;
  list->width = width;
  list->height = height;
  list->fg = (uint32_t)args[5].u_int;
  list->bg = (uint32_t)args[6].u_int;
  list->sel_fg = args[7].u_int >= 0 ? (uint32_t)args[7].u_int : list->bg;
  list->sel_bg = args[8].u_int >= 0 ? (uint32_t)args[8].u_int : list->fg;

  int sel = args[9].u_int;
  if (sel < 0)
    sel = 0;
  if (list->item_count > 0 && sel >= list->item_count)
    sel = list->item_count - 1;
  list->decorations = args[15].u_bool;
  list->multiline = args[16].u_bool;
  list->wrap = args[17].u_bool;
  list->selected = sel;
  int vis_h = list->decorations ? (height - 2) : height;
  if (!list_is_multirow(list)) {
    list->scroll = (sel >= vis_h) ? sel - vis_h + 1 : 0;
  } else {
    list->scroll = 0;
    while (list->scroll < sel) {
      int rows = 0;
      for (int i = list->scroll; i <= sel; i++)
        rows += list_item_rows(list, i);
      if (rows <= vis_h)
        break;
      list->scroll++;
    }
  }

  list->last_selected = -1;
  list->last_scroll = -1;
  list->on_change = args[10].u_obj;

  // Arrow: store raw UTF-8 bytes of the first character.
  list->arrow_len = 0;
  if (args[11].u_obj != mp_const_none) {
    size_t alen;
    const char *astr = mp_obj_str_get_data(args[11].u_obj, &alen);
    if (alen > 0) {
      unsigned char first = (unsigned char)astr[0];
      int clen = (first < 0x80)   ? 1
                 : (first < 0xE0) ? 2
                 : (first < 0xF0) ? 3
                                  : 4;
      if (clen > (int)alen)
        clen = (int)alen;
      memcpy(list->arrow, astr, clen);
      list->arrow_len = clen;
    }
  }
  list->left_pad = args[12].u_int > 0 ? args[12].u_int : 0;
  list->title = args[14].u_obj;
  return list;
}

// ─── VTInput
// ─────────────────────────────────────────────────────────────────

static mp_obj_t vttui_input_draw(mp_obj_t self_in) {
  vttui_input_obj_t *self = MP_OBJ_TO_PTR(self_in);
  size_t label_len;
  const char *label = mp_obj_str_get_data(self->label_obj, &label_len);

  int content_row = self->decorations ? self->abs_y + 1 : self->abs_y;

  // With decorations:    | label [input] |   (inner_w = width-2, input_area =
  // inner_w - label - 3) Without decorations: label [input]        (input_area
  // = width - label - 1)
  int label_gap = label_len > 0 ? 1 : 0;
  int input_area;
  if (self->decorations) {
    int inner_w = self->width - 2;
    input_area = inner_w - (int)label_len - label_gap - 2;
  } else {
    input_area = self->width - (int)label_len - label_gap;
  }
  if (input_area < 1)
    input_area = 1;

  int start = (self->buf_len > input_area) ? self->buf_len - input_area : 0;
  int visible = self->buf_len - start;

  if (self->decorations) {
    // Row 0: ┌───┐
    int inner_w = self->width - 2;
    vttui_begin(self->abs_x, self->abs_y, self->fg, self->bg, self->bold);
    vttui_write("\xe2\x94\x8c", 3);
    write_repeat_str("\xe2\x94\x80", 3, inner_w);
    vttui_write("\xe2\x94\x90", 3);
    vttui_write_str("\033[0m");
  }

  // Content row
  vttui_begin(self->abs_x, content_row, self->fg, self->bg, self->bold);
  if (self->decorations)
    vttui_write("\xe2\x94\x82 ", 4);
  // With decorations: bold is for borders, so label is plain.
  // Without decorations: bold applies to the label itself.
  vttui_sgr(self->fg, self->bg, self->decorations ? false : self->bold);
  vttui_write(label, label_len);
  if (label_gap)
    vttui_write(" ", 1);

  // Switch to input_bg for the text area
  vttui_sgr(self->fg, self->input_bg, false);
  if (self->secret) {
    write_repeat_str("*", 1, visible);
  } else {
    vttui_write(self->buf + start, (size_t)visible);
  }
  write_spaces(input_area - visible);
  if (self->decorations) {
    // Restore box color for trailing border
    vttui_sgr(self->fg, self->bg, self->bold);
    vttui_write(" \xe2\x94\x82", 4);
  }
  vttui_write_str("\033[0m");

  if (self->decorations) {
    // Row 2: └───┘
    int inner_w = self->width - 2;
    vttui_begin(self->abs_x, self->abs_y + 2, self->fg, self->bg, self->bold);
    vttui_write("\xe2\x94\x94", 3);
    write_repeat_str("\xe2\x94\x80", 3, inner_w);
    vttui_write("\xe2\x94\x98", 3);
    vttui_write_str("\033[0m");
  }

  // Park cursor at end of typed text
  int cursor_x = self->decorations
                     ? self->abs_x + 2 + (int)label_len + label_gap + visible
                     : self->abs_x + (int)label_len + label_gap + visible;
  vttui_move(cursor_x, content_row);

  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_input_draw_obj, vttui_input_draw);

static mp_obj_t vttui_input_push(mp_obj_t self_in, mp_obj_t char_obj) {
  vttui_input_obj_t *self = MP_OBJ_TO_PTR(self_in);
  size_t clen;
  const char *c = mp_obj_str_get_data(char_obj, &clen);
  if (clen > 0 && self->buf_len < VTTUI_INPUT_MAX) {
    unsigned char ch = (unsigned char)c[0];
    if (ch >= 0x20 && ch != 0x7f)
      self->buf[self->buf_len++] = (char)ch;
  }
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(vttui_input_push_obj, vttui_input_push);

static mp_obj_t vttui_input_backspace(mp_obj_t self_in) {
  vttui_input_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (self->buf_len > 0)
    self->buf_len--;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_input_backspace_obj,
                                 vttui_input_backspace);

static mp_obj_t vttui_input_clear(mp_obj_t self_in) {
  ((vttui_input_obj_t *)MP_OBJ_TO_PTR(self_in))->buf_len = 0;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_input_clear_obj, vttui_input_clear);

static mp_obj_t vttui_input_set(mp_obj_t self_in, mp_obj_t text_obj) {
  vttui_input_obj_t *self = MP_OBJ_TO_PTR(self_in);
  size_t len;
  const char *s = mp_obj_str_get_data(text_obj, &len);
  if (len > VTTUI_INPUT_MAX)
    len = VTTUI_INPUT_MAX;
  memcpy(self->buf, s, len);
  self->buf_len = (int)len;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(vttui_input_set_obj, vttui_input_set);

static const mp_rom_map_elem_t vttui_input_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_draw), MP_ROM_PTR(&vttui_input_draw_obj)},
    {MP_ROM_QSTR(MP_QSTR_push), MP_ROM_PTR(&vttui_input_push_obj)},
    {MP_ROM_QSTR(MP_QSTR_backspace), MP_ROM_PTR(&vttui_input_backspace_obj)},
    {MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&vttui_input_clear_obj)},
    {MP_ROM_QSTR(MP_QSTR_set), MP_ROM_PTR(&vttui_input_set_obj)},
};
static MP_DEFINE_CONST_DICT(vttui_input_locals_dict,
                            vttui_input_locals_dict_table);

static void vttui_input_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
  if (dest[0] != MP_OBJ_NULL)
    return;
  vttui_input_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (attr == MP_QSTR_value) {
    dest[0] = mp_obj_new_str(self->buf, (size_t)self->buf_len);
    return;
  }
  mp_map_elem_t *elem = mp_map_lookup((mp_map_t *)&vttui_input_locals_dict.map,
                                      MP_OBJ_NEW_QSTR(attr), MP_MAP_LOOKUP);
  if (elem) {
    dest[0] = elem->value;
    dest[1] = self_in;
  }
}

MP_DEFINE_CONST_OBJ_TYPE(vttui_input_type, MP_QSTR_VTInput, MP_TYPE_FLAG_NONE,
                         attr, vttui_input_attr, locals_dict,
                         &vttui_input_locals_dict);

// Shared arg table for make_input on VTTUI and VTWindow.
static const mp_arg_t make_input_args[] = {
    {MP_QSTR_label, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
    {MP_QSTR_x, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_y, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_width, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 40}},
    {MP_QSTR_fg, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1}},
    {MP_QSTR_bg, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1}},
    {MP_QSTR_bold, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = true}},
    {MP_QSTR_secret, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false}},
    {MP_QSTR_input_bg, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_align, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none}},
    {MP_QSTR_decorations, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = true}},
    {MP_QSTR_value, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none}},
};

static vttui_input_obj_t *create_input(mp_arg_val_t *args, int abs_x,
                                       int abs_y) {
  vttui_input_obj_t *inp = m_new_obj(vttui_input_obj_t);
  inp->base.type = &vttui_input_type;
  inp->abs_x = abs_x;
  inp->abs_y = abs_y;
  inp->width = args[3].u_int > 4 ? args[3].u_int : 4;
  inp->fg = args[4].u_int < 0 ? 257 : (uint32_t)args[4].u_int;
  inp->bg = args[5].u_int < 0 ? 256 : (uint32_t)args[5].u_int;
  inp->bold = args[6].u_bool;
  inp->secret = args[7].u_bool;
  inp->input_bg = (uint32_t)args[8].u_int;
  inp->label_obj = args[0].u_obj;
  inp->buf_len = 0;
  if (args[11].u_obj != mp_const_none) {
    size_t vlen;
    const char *vstr = mp_obj_str_get_data(args[11].u_obj, &vlen);
    if (vlen > VTTUI_INPUT_MAX)
      vlen = VTTUI_INPUT_MAX;
    memcpy(inp->buf, vstr, vlen);
    inp->buf_len = (int)vlen;
  }
  inp->decorations = args[10].u_bool;
  return inp;
}

// ─── VTBlock
// ─────────────────────────────────────────────────────────────────

// Write text, re-injecting the baseline color after every \033[0m reset so
// that \x1b[0m mid-line doesn't leave a black background on subsequent chars.
static void write_block_text(const char *text, int len, uint32_t fg,
                             uint32_t bg) {
  const char *seg = text;
  const char *p = text;
  const char *end = text + len;

  while (p < end) {
    // Detect \033[0m (4 bytes) or \033[m (3 bytes)
    if (p[0] == '\033' && (p + 1) < end && p[1] == '[') {
      if ((p + 3) < end && p[2] == '0' && p[3] == 'm') {
        vttui_write(seg, (size_t)(p + 4 - seg));
        vttui_sgr(fg, bg, false);
        seg = p + 4;
        p = seg;
        continue;
      }
      if ((p + 2) < end && p[2] == 'm') {
        vttui_write(seg, (size_t)(p + 3 - seg));
        vttui_sgr(fg, bg, false);
        seg = p + 3;
        p = seg;
        continue;
      }
    }
    p++;
  }
  if (seg < end)
    vttui_write(seg, (size_t)(end - seg));
}

// Replay ANSI escape sequences from [start, end) to restore color state,
// mirroring write_block_text's handling of reset sequences.
static void replay_ansi_state(const char *start, const char *end, uint32_t fg,
                              uint32_t bg) {
  const char *p = start;
  while (p < end) {
    if (p[0] == '\033' && p + 1 < end && p[1] == '[') {
      if ((p + 3) < end && p[2] == '0' && p[3] == 'm') {
        vttui_sgr(fg, bg, false);
        p += 4;
      } else if ((p + 2) < end && p[2] == 'm') {
        vttui_sgr(fg, bg, false);
        p += 3;
      } else {
        const char *seq = p;
        p += 2;
        while (p < end &&
               ((unsigned char)*p < 0x40 || (unsigned char)*p > 0x7e))
          p++;
        if (p < end)
          p++;
        vttui_write(seq, (size_t)(p - seq));
      }
    } else {
      p++;
    }
  }
}

// Returns the byte-length of the next display chunk containing up to `width`
// visible characters.  ANSI escape sequences are skipped (not counted).
// Multi-byte UTF-8 sequences count as one visible character. If out_vis is
// non-NULL, the number of visible characters actually consumed (<= width)
// is written there -- callers that need to know how many screen columns
// the returned byte range occupies (as opposed to just its byte length)
// can't get that any other way, since escape bytes inflate byte length
// without occupying any columns.
static int next_chunk_len(const char *s, int byte_len, int width, int *out_vis) {
  int vis = 0, i = 0;
  while (i < byte_len) {
    unsigned char c = (unsigned char)s[i];
    if (c == '\033') {
      i++;
      if (i < byte_len && s[i] == '[') {
        i++;
        while (i < byte_len &&
               ((unsigned char)s[i] < 0x40 || (unsigned char)s[i] > 0x7e))
          i++;
        if (i < byte_len)
          i++;
      }
    } else if ((c & 0xC0) == 0x80) {
      i++; // UTF-8 continuation byte — skip
    } else {
      vis++;
      i++;
      if ((c & 0xC0) == 0xC0) // multi-byte lead: skip continuations
        while (i < byte_len && ((unsigned char)s[i] & 0xC0) == 0x80)
          i++;
      if (vis >= width)
        break;
    }
  }
  if (out_vis)
    *out_vis = vis;
  return i;
}

// Count how many display rows a single logical line occupies.
static int line_display_rows(const char *s, int len, bool wrap, int width) {
  if (!wrap || width <= 0 || len == 0)
    return 1;
  int rows = 0, remaining = len;
  const char *pos = s;
  while (remaining > 0) {
    int cl = next_chunk_len(pos, remaining, width, NULL);
    if (cl == 0)
      break;
    rows++;
    pos += cl;
    remaining -= cl;
  }
  return rows > 0 ? rows : 1;
}

static mp_obj_t vttui_block_draw(mp_obj_t self_in) {
  vttui_block_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (!self->dirty)
    return mp_const_none;
  self->dirty = false;

  size_t text_len;
  const char *text = mp_obj_str_get_data(self->text_obj, &text_len);
  const char *end = text + text_len;

  // Count total display rows for scroll offset
  int skip_rows = 0;
  if (self->scroll && self->height > 0) {
    int total = 0;
    const char *ls = text;
    for (const char *p = text; p <= end; p++) {
      if (p == end || *p == '\n') {
        if (p == end && p == ls)
          break; // skip trailing empty segment
        total += line_display_rows(ls, (int)(p - ls), self->wrap, self->width);
        ls = p + 1;
      }
    }
    skip_rows = total - self->height;
    if (skip_rows < 0)
      skip_rows = 0;
  }

  // Render, skipping the first `skip_rows` display rows
  int display_row = 0; // display rows consumed so far
  int screen_row = 0;  // rows rendered to screen
  const char *ls = text;

  for (const char *p = text; p <= end; p++) {
    if (self->height > 0 && screen_row >= self->height)
      break;
    if (p == end || *p == '\n') {
      int line_len = (int)(p - ls);
      if (p == end && line_len == 0)
        break; // skip trailing empty segment

      if (self->wrap && self->width > 0) {
        // Emit wrapped chunks
        int remaining = line_len;
        const char *pos = ls;
        bool first_chunk = true;
        while (first_chunk || remaining > 0) {
          int cl = remaining > 0
                       ? next_chunk_len(pos, remaining, self->width, NULL)
                       : 0;
          if (display_row >= skip_rows) {
            if (self->height > 0 && screen_row >= self->height)
              break;
            vttui_move(self->abs_x, self->abs_y + screen_row);
            vttui_sgr(self->fg, self->bg, false);
            if (self->width > 0)
              write_spaces(self->width);
            vttui_move(self->abs_x, self->abs_y + screen_row);
            vttui_sgr(self->fg, self->bg, false);
            if (!first_chunk)
              replay_ansi_state(ls, pos, self->fg, self->bg);
            write_block_text(pos, cl, self->fg, self->bg);
            screen_row++;
          }
          display_row++;
          if (cl == 0)
            break;
          pos += cl;
          remaining -= cl;
          first_chunk = false;
        }
      } else {
        if (display_row >= skip_rows) {
          vttui_move(self->abs_x, self->abs_y + screen_row);
          vttui_sgr(self->fg, self->bg, false);
          if (self->width > 0)
            write_spaces(self->width);
          vttui_move(self->abs_x, self->abs_y + screen_row);
          vttui_sgr(self->fg, self->bg, false);
          write_block_text(ls, line_len, self->fg, self->bg);
          screen_row++;
        }
        display_row++;
      }

      ls = p + 1;
    }
  }

  // Fill remaining rows with background color
  if (self->height > 0 && self->width > 0) {
    while (screen_row < self->height) {
      vttui_move(self->abs_x, self->abs_y + screen_row);
      vttui_sgr(self->fg, self->bg, false);
      write_spaces(self->width);
      screen_row++;
    }
  }

  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_block_draw_obj, vttui_block_draw);

static mp_obj_t vttui_block_set(mp_obj_t self_in, mp_obj_t text_obj) {
  vttui_block_obj_t *self = MP_OBJ_TO_PTR(self_in);
  self->text_obj = text_obj;
  self->dirty = true;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(vttui_block_set_obj, vttui_block_set);

static mp_obj_t vttui_block_push(mp_obj_t self_in, mp_obj_t text_obj) {
  vttui_block_obj_t *self = MP_OBJ_TO_PTR(self_in);

  // Append new text
  self->text_obj = mp_binary_op(MP_BINARY_OP_ADD, self->text_obj, text_obj);

  // Trim to last `height` lines so memory stays bounded
  if (self->scroll && self->height > 0) {
    size_t len;
    const char *text = mp_obj_str_get_data(self->text_obj, &len);
    int nl_count = 0;
    for (size_t i = 0; i < len; i++) {
      if (text[i] == '\n')
        nl_count++;
    }
    // If text ends with \n the trailing empty segment isn't a visible line
    int visible_lines =
        (len > 0 && text[len - 1] == '\n') ? nl_count : nl_count + 1;
    int skip = visible_lines - self->height;
    if (skip > 0) {
      int skipped = 0;
      for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n') {
          if (++skipped >= skip) {
            self->text_obj = mp_obj_new_str(text + i + 1, len - i - 1);
            break;
          }
        }
      }
    }
  }

  self->dirty = true;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(vttui_block_push_obj, vttui_block_push);

static mp_obj_t vttui_block_invalidate(mp_obj_t self_in) {
  ((vttui_block_obj_t *)MP_OBJ_TO_PTR(self_in))->dirty = true;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_block_invalidate_obj,
                                 vttui_block_invalidate);

static const mp_rom_map_elem_t vttui_block_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_draw), MP_ROM_PTR(&vttui_block_draw_obj)},
    {MP_ROM_QSTR(MP_QSTR_set), MP_ROM_PTR(&vttui_block_set_obj)},
    {MP_ROM_QSTR(MP_QSTR_push), MP_ROM_PTR(&vttui_block_push_obj)},
    {MP_ROM_QSTR(MP_QSTR_invalidate), MP_ROM_PTR(&vttui_block_invalidate_obj)},
};
static MP_DEFINE_CONST_DICT(vttui_block_locals_dict,
                            vttui_block_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(vttui_block_type, MP_QSTR_VTBlock, MP_TYPE_FLAG_NONE,
                         locals_dict, &vttui_block_locals_dict);

static const mp_arg_t make_block_args[] = {
    {MP_QSTR_text, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
    {MP_QSTR_x, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_y, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_fg, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1}},
    {MP_QSTR_bg, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1}},
    {MP_QSTR_width, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_height, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_scroll, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false}},
    {MP_QSTR_wrap, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false}},
};

static vttui_block_obj_t *create_block(mp_arg_val_t *args, int abs_x, int abs_y,
                                       uint32_t fg, uint32_t bg, int width,
                                       int height, bool scroll, bool wrap) {
  vttui_block_obj_t *blk = m_new_obj(vttui_block_obj_t);
  blk->base.type = &vttui_block_type;
  blk->abs_x = abs_x;
  blk->abs_y = abs_y;
  blk->fg = fg;
  blk->bg = bg;
  blk->width = width > 0 ? width : 0;
  blk->height = height > 0 ? height : 0;
  blk->scroll = scroll;
  blk->wrap = wrap;
  blk->text_obj = args[0].u_obj;
  blk->dirty = true;
  return blk;
}

// ─── VTPager
// ─────────────────────────────────────────────────────────────────
//
// A scrollable, continuously-reflowing text pager (like `less`) built from
// a list of (text, value) segments -- value is None for plain text, or any
// Python object identifying a link's destination for a "link" segment.
// Unlike VTList (which navigates whole items and can't split one across a
// partial scroll), VTPager wraps ALL segments together into one continuous
// stream of rows, so a link's text can sit inline mid-paragraph and wrap
// together with the plain text around it -- a row's runs[] can span
// several segments. up()/down() scroll one row at a time; next_link()/
// prev_link() move a separate "current link" cursor (wrapping at the
// ends) and auto-scroll just enough to bring it on screen; .current_link
// reads back that link's `value` for the caller to act on (e.g. on Enter).

typedef struct _vttui_pager_run_t {
  int segment;    // index into self->segments
  int seg_offset; // byte offset within that segment's text
  int seg_len;    // byte length of this run
  int vis_width;  // screen columns this run occupies (from next_chunk_len)
  bool is_link;
} vttui_pager_run_t;

typedef struct _vttui_pager_obj_t {
  mp_obj_base_t base;
  mp_obj_t segments; // Python list of (text, value) tuples, kept alive for GC
  int segment_count;

  int x, y, width, height;
  uint32_t fg, bg;
  uint32_t link_fg, link_bg;
  uint32_t cur_fg, cur_bg;

  vttui_pager_run_t *runs;
  int run_count, run_cap;

  int *row_starts; // size row_count+1: row_starts[r] = index into runs[] where row r begins
  int row_count, row_cap;

  int *link_segments; // segment index of each link, in document order
  int *link_rows;      // parallel: row index of that link's first run
  int link_count, link_cap;
  int current_link; // index into link_segments/link_rows, or -1 = none selected yet

  int scroll_row;
  int last_scroll_row;
  int last_current_link;
} vttui_pager_obj_t;

static void pager_get_segment(vttui_pager_obj_t *self, int i, const char **text,
                              size_t *len, mp_obj_t *value) {
  mp_obj_t item =
      mp_obj_subscr(self->segments, MP_OBJ_NEW_SMALL_INT(i), MP_OBJ_SENTINEL);
  mp_obj_t *items;
  mp_obj_get_array_fixed_n(item, 2, &items);
  *text = mp_obj_str_get_data(items[0], len);
  *value = items[1];
}

static void pager_ensure_run_cap(vttui_pager_obj_t *self, int needed) {
  if (needed <= self->run_cap)
    return;
  int new_cap = self->run_cap ? self->run_cap * 2 : 64;
  while (new_cap < needed)
    new_cap *= 2;
  self->runs = m_renew(vttui_pager_run_t, self->runs, self->run_cap, new_cap);
  self->run_cap = new_cap;
}

static void pager_ensure_row_cap(vttui_pager_obj_t *self, int needed) {
  if (needed <= self->row_cap)
    return;
  int new_cap = self->row_cap ? self->row_cap * 2 : 64;
  while (new_cap < needed)
    new_cap *= 2;
  self->row_starts = m_renew(int, self->row_starts, self->row_cap, new_cap);
  self->row_cap = new_cap;
}

static void pager_ensure_link_cap(vttui_pager_obj_t *self, int needed) {
  if (needed <= self->link_cap)
    return;
  int new_cap = self->link_cap ? self->link_cap * 2 : 16;
  while (new_cap < needed)
    new_cap *= 2;
  self->link_segments =
      m_renew(int, self->link_segments, self->link_cap, new_cap);
  self->link_rows = m_renew(int, self->link_rows, self->link_cap, new_cap);
  self->link_cap = new_cap;
}

// Builds self->runs/row_starts/link_segments/link_rows by walking every
// segment's text as one continuous stream, wrapping to self->width columns
// and breaking rows on literal '\n' bytes -- segments only affect coloring
// (is_link) and where link-navigation can land, never where wrapping
// happens, so a link's text reflows exactly like the plain text around it.
static void pager_build_index(vttui_pager_obj_t *self) {
  int text_width = self->width > 0 ? self->width : 1;

  self->run_count = 0;
  self->row_count = 0;
  self->link_count = 0;
  pager_ensure_row_cap(self, 1);
  self->row_starts[0] = 0;

  int seg = 0, off = 0;
  int row_width_left = text_width;
  bool row_has_content = false;
  int last_link_seg = -1;

  const char *text = NULL;
  size_t seg_len_total = 0;
  mp_obj_t value = mp_const_none;
  bool is_link = false;

  // Iteration cap purely as a last-resort circuit breaker against an
  // unforeseen bug looping forever on real hardware (a hang here needs a
  // physical reset, unlike almost anything else this widget could get
  // wrong) -- not expected to ever trigger given realistic article sizes.
  long guard = 0;
  const long guard_max = 20000000L;

  while (seg < self->segment_count) {
    if (++guard > guard_max)
      break;

    if (text == NULL) {
      pager_get_segment(self, seg, &text, &seg_len_total, &value);
      is_link = (value != mp_const_none);
    }
    int length = (int)seg_len_total;

    if (off >= length) {
      seg++;
      off = 0;
      text = NULL;
      continue;
    }

    if (row_width_left <= 0) {
      self->row_count++;
      pager_ensure_row_cap(self, self->row_count + 1);
      self->row_starts[self->row_count] = self->run_count;
      row_width_left = text_width;
      row_has_content = false;
      continue;
    }

    // Find the next literal '\n' within this segment at/after `off`.
    int nl_pos = -1;
    for (int i = off; i < length; i++) {
      if (text[i] == '\n') {
        nl_pos = i;
        break;
      }
    }
    int seg_limit = (nl_pos >= 0) ? nl_pos : length;
    int avail = seg_limit - off;

    if (avail > 0) {
      int vis = 0;
      int cl = next_chunk_len(text + off, avail, row_width_left, &vis);
      if (cl > 0) {
        pager_ensure_run_cap(self, self->run_count + 1);
        vttui_pager_run_t *run = &self->runs[self->run_count++];
        run->segment = seg;
        run->seg_offset = off;
        run->seg_len = cl;
        run->vis_width = vis;
        run->is_link = is_link;

        if (is_link && seg != last_link_seg) {
          pager_ensure_link_cap(self, self->link_count + 1);
          self->link_segments[self->link_count] = seg;
          self->link_rows[self->link_count] = self->row_count;
          self->link_count++;
          last_link_seg = seg;
        }

        row_has_content = true;
        off += cl;
        row_width_left -= vis;
        continue;
      }
    }

    if (nl_pos == off) {
      off += 1; // consume the newline itself
      self->row_count++;
      pager_ensure_row_cap(self, self->row_count + 1);
      self->row_starts[self->row_count] = self->run_count;
      row_width_left = text_width;
      row_has_content = false;
      continue;
    }

    // Nothing consumable left in this segment (avail == 0, no newline
    // right here) -- move on to the next segment, same row continues.
    seg++;
    off = 0;
    text = NULL;
  }

  if (row_has_content || self->row_count == 0) {
    self->row_count++;
    pager_ensure_row_cap(self, self->row_count + 1);
    self->row_starts[self->row_count] = self->run_count;
  }

  self->scroll_row = 0;
  self->last_scroll_row = -1;
  self->current_link = -1;
  self->last_current_link = -1;
}

static void render_pager_row(vttui_pager_obj_t *self, int row_idx, int y) {
  int run_start = self->row_starts[row_idx];
  int run_end = self->row_starts[row_idx + 1];

  vttui_begin(self->x, y, self->fg, self->bg, false);
  int col = 0;
  for (int ri = run_start; ri < run_end; ri++) {
    vttui_pager_run_t *run = &self->runs[ri];
    const char *text;
    size_t seglen;
    mp_obj_t value;
    pager_get_segment(self, run->segment, &text, &seglen, &value);

    bool is_current = self->current_link >= 0 &&
                      run->segment == self->link_segments[self->current_link];
    uint32_t rfg = run->is_link ? (is_current ? self->cur_fg : self->link_fg)
                                : self->fg;
    uint32_t rbg = run->is_link ? (is_current ? self->cur_bg : self->link_bg)
                                : self->bg;

    vttui_sgr(rfg, rbg, false);
    write_block_text(text + run->seg_offset, run->seg_len, rfg, rbg);
    col += run->vis_width;
  }

  int remaining = self->width - col;
  if (remaining > 0) {
    vttui_sgr(self->fg, self->bg, false);
    write_spaces(remaining);
  }
  vttui_write_str("\033[0m");
}

static mp_obj_t vttui_pager_draw(mp_obj_t self_in) {
  vttui_pager_obj_t *self = MP_OBJ_TO_PTR(self_in);
  bool first_draw = (self->last_scroll_row == -1);
  bool changed = first_draw || self->scroll_row != self->last_scroll_row ||
                self->current_link != self->last_current_link;
  if (!changed)
    return mp_const_none;

  for (int i = 0; i < self->height; i++) {
    int row_idx = self->scroll_row + i;
    int y = self->y + i;
    if (row_idx < self->row_count) {
      render_pager_row(self, row_idx, y);
    } else {
      vttui_begin(self->x, y, self->fg, self->bg, false);
      write_spaces(self->width);
      vttui_write_str("\033[0m");
    }
  }

  self->last_scroll_row = self->scroll_row;
  self->last_current_link = self->current_link;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_pager_draw_obj, vttui_pager_draw);

static mp_obj_t vttui_pager_up(mp_obj_t self_in) {
  vttui_pager_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (self->scroll_row > 0)
    self->scroll_row--;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_pager_up_obj, vttui_pager_up);

static mp_obj_t vttui_pager_down(mp_obj_t self_in) {
  vttui_pager_obj_t *self = MP_OBJ_TO_PTR(self_in);
  int max_scroll =
      self->row_count > self->height ? self->row_count - self->height : 0;
  if (self->scroll_row < max_scroll)
    self->scroll_row++;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_pager_down_obj, vttui_pager_down);

// Scrolls a full page (self->height rows) at a time -- same clamping
// shape pager_scroll_to_row() below uses.
static mp_obj_t vttui_pager_page_up(mp_obj_t self_in) {
  vttui_pager_obj_t *self = MP_OBJ_TO_PTR(self_in);
  self->scroll_row -= self->height;
  if (self->scroll_row < 0)
    self->scroll_row = 0;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_pager_page_up_obj, vttui_pager_page_up);

static mp_obj_t vttui_pager_page_down(mp_obj_t self_in) {
  vttui_pager_obj_t *self = MP_OBJ_TO_PTR(self_in);
  int max_scroll =
      self->row_count > self->height ? self->row_count - self->height : 0;
  self->scroll_row += self->height;
  if (self->scroll_row > max_scroll)
    self->scroll_row = max_scroll;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_pager_page_down_obj, vttui_pager_page_down);

// Scrolls just enough (never more) to bring row_idx into the visible
// window -- same clamping shape as list_set_selected()'s multirow branch.
static void pager_scroll_to_row(vttui_pager_obj_t *self, int row_idx) {
  int max_scroll =
      self->row_count > self->height ? self->row_count - self->height : 0;
  if (row_idx < self->scroll_row) {
    self->scroll_row = row_idx;
  } else if (row_idx >= self->scroll_row + self->height) {
    self->scroll_row = row_idx - self->height + 1;
  }
  if (self->scroll_row < 0)
    self->scroll_row = 0;
  if (self->scroll_row > max_scroll)
    self->scroll_row = max_scroll;
}

static mp_obj_t vttui_pager_next_link(mp_obj_t self_in) {
  vttui_pager_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (self->link_count == 0)
    return mp_const_none;
  self->current_link = (self->current_link < 0)
                           ? 0
                           : (self->current_link + 1) % self->link_count;
  pager_scroll_to_row(self, self->link_rows[self->current_link]);
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_pager_next_link_obj,
                                 vttui_pager_next_link);

static mp_obj_t vttui_pager_prev_link(mp_obj_t self_in) {
  vttui_pager_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (self->link_count == 0)
    return mp_const_none;
  self->current_link = (self->current_link < 0)
                           ? self->link_count - 1
                           : (self->current_link - 1 + self->link_count) %
                                 self->link_count;
  pager_scroll_to_row(self, self->link_rows[self->current_link]);
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_pager_prev_link_obj,
                                 vttui_pager_prev_link);

static const mp_rom_map_elem_t vttui_pager_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_draw), MP_ROM_PTR(&vttui_pager_draw_obj)},
    {MP_ROM_QSTR(MP_QSTR_up), MP_ROM_PTR(&vttui_pager_up_obj)},
    {MP_ROM_QSTR(MP_QSTR_down), MP_ROM_PTR(&vttui_pager_down_obj)},
    {MP_ROM_QSTR(MP_QSTR_page_up), MP_ROM_PTR(&vttui_pager_page_up_obj)},
    {MP_ROM_QSTR(MP_QSTR_page_down), MP_ROM_PTR(&vttui_pager_page_down_obj)},
    {MP_ROM_QSTR(MP_QSTR_next_link), MP_ROM_PTR(&vttui_pager_next_link_obj)},
    {MP_ROM_QSTR(MP_QSTR_prev_link), MP_ROM_PTR(&vttui_pager_prev_link_obj)},
};
static MP_DEFINE_CONST_DICT(vttui_pager_locals_dict,
                            vttui_pager_locals_dict_table);

static void vttui_pager_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
  if (dest[0] != MP_OBJ_NULL)
    return; // no settable attributes
  vttui_pager_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (attr == MP_QSTR_current_link) {
    if (self->current_link < 0) {
      dest[0] = mp_const_none;
    } else {
      const char *text;
      size_t len;
      mp_obj_t value;
      pager_get_segment(self, self->link_segments[self->current_link], &text,
                        &len, &value);
      dest[0] = value;
    }
    return;
  }
  mp_map_elem_t *elem =
      mp_map_lookup((mp_map_t *)&vttui_pager_locals_dict.map,
                    MP_OBJ_NEW_QSTR(attr), MP_MAP_LOOKUP);
  if (elem) {
    dest[0] = elem->value;
    dest[1] = self_in;
  }
}

MP_DEFINE_CONST_OBJ_TYPE(vttui_pager_type, MP_QSTR_VTPager, MP_TYPE_FLAG_NONE,
                         attr, vttui_pager_attr, locals_dict,
                         &vttui_pager_locals_dict);

static const mp_arg_t make_pager_args[] = {
    {MP_QSTR_segments, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
    {MP_QSTR_x, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_y, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_width, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_height, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_fg, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_bg, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_link_fg, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1}},
    {MP_QSTR_link_bg, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1}},
    {MP_QSTR_cur_fg, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1}},
    {MP_QSTR_cur_bg, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1}},
};

static vttui_pager_obj_t *create_pager(mp_arg_val_t *args, int abs_x, int abs_y,
                                       int width, int height) {
  vttui_pager_obj_t *pager = m_new_obj(vttui_pager_obj_t);
  pager->base.type = &vttui_pager_type;
  pager->segments = args[0].u_obj;
  pager->segment_count = mp_obj_get_int(mp_obj_len(pager->segments));
  pager->x = abs_x;
  pager->y = abs_y;
  pager->width = width;
  pager->height = height;
  pager->fg = (uint32_t)args[5].u_int;
  pager->bg = (uint32_t)args[6].u_int;
  pager->link_fg = args[7].u_int >= 0 ? (uint32_t)args[7].u_int : pager->fg;
  pager->link_bg = args[8].u_int >= 0 ? (uint32_t)args[8].u_int : pager->bg;
  pager->cur_fg = args[9].u_int >= 0 ? (uint32_t)args[9].u_int : pager->bg;
  pager->cur_bg = args[10].u_int >= 0 ? (uint32_t)args[10].u_int : pager->fg;

  pager->runs = NULL;
  pager->run_count = 0;
  pager->run_cap = 0;
  pager->row_starts = NULL;
  pager->row_count = 0;
  pager->row_cap = 0;
  pager->link_segments = NULL;
  pager->link_rows = NULL;
  pager->link_count = 0;
  pager->link_cap = 0;

  pager_build_index(pager);

  return pager;
}

static mp_obj_t vttui_make_pager(size_t n_args, const mp_obj_t *pos_args,
                                 mp_map_t *kw_args) {
  mp_arg_val_t args[MP_ARRAY_SIZE(make_pager_args)];
  mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                   MP_ARRAY_SIZE(make_pager_args), make_pager_args, args);
  return MP_OBJ_FROM_PTR(create_pager(args, args[1].u_int, args[2].u_int,
                                      args[3].u_int, args[4].u_int));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(vttui_make_pager_obj, 1, vttui_make_pager);

// ─── VTDialog
// ─────────────────────────────────────────────────────────────────

static void render_dialog_buttons(vttui_dialog_obj_t *self, int inner_x,
                                  int inner_y, int inner_w, int inner_h) {
  size_t b1len;
  const char *b1 = mp_obj_str_get_data(self->btn1, &b1len);
  bool has_btn2 = (self->btn2 != mp_const_none);
  int by = inner_y + inner_h - 1;

  if (!has_btn2) {
    // Single button, centered. It's always "selected" since there's nothing
    // else to navigate to.
    int total_w = (int)b1len + 4;
    int bx = inner_x + (inner_w - total_w) / 2;
    if (bx < inner_x)
      bx = inner_x;

    vttui_sgr(self->sel_fg, self->sel_bg, false);
    vttui_move(bx, by);
    vttui_write_str("[ ");
    vttui_write(b1, b1len);
    vttui_write_str(" ]");
    vttui_write_str("\033[0m");
    return;
  }

  size_t b2len;
  const char *b2 = mp_obj_str_get_data(self->btn2, &b2len);

  // Each button is "[ label ]" (4 overhead), gap of 3 between them.
  int total_w = (int)b1len + 4 + 3 + (int)b2len + 4;
  int bx = inner_x + (inner_w - total_w) / 2;
  if (bx < inner_x)
    bx = inner_x;

  uint32_t fg0 = (self->selected == 0) ? self->sel_fg : self->fg;
  uint32_t bg0 = (self->selected == 0) ? self->sel_bg : self->bg;
  uint32_t fg1 = (self->selected == 1) ? self->sel_fg : self->fg;
  uint32_t bg1 = (self->selected == 1) ? self->sel_bg : self->bg;

  vttui_sgr(fg0, bg0, false);
  vttui_move(bx, by);
  vttui_write_str("[ ");
  vttui_write(b1, b1len);
  vttui_write_str(" ]");
  vttui_write_str("\033[0m");

  vttui_sgr(self->fg, self->bg, false);
  vttui_write_str("   ");

  vttui_sgr(fg1, bg1, false);
  vttui_write_str("[ ");
  vttui_write(b2, b2len);
  vttui_write_str(" ]");
  vttui_write_str("\033[0m");
}

static mp_obj_t vttui_dialog_draw(mp_obj_t self_in) {
  vttui_dialog_obj_t *self = MP_OBJ_TO_PTR(self_in);
  bool first_draw = (self->last_selected == -1);
  bool sel_changed = (self->selected != self->last_selected);

  if (!first_draw && !sel_changed)
    return mp_const_none;

  int inner_x, inner_y, inner_w, inner_h;
  if (self->decorations) {
    inner_x = self->x + 1;
    inner_y = self->y + 1;
    inner_w = self->width - 2;
    inner_h = self->height - 2;
  } else {
    inner_x = self->x;
    inner_y = self->y;
    inner_w = self->width;
    inner_h = self->height;
  }

  if (first_draw) {
    // Clear inner area
    for (int row = 0; row < inner_h; row++)
      render_label_raw(inner_x, inner_y + row, "", 0, inner_w, self->fg,
                       self->bg, false, 0);

    // Border with title
    if (self->decorations) {
      int x = self->x, y = self->y, w = self->width, h = self->height;
      vttui_begin(x, y, self->fg, self->bg, false);
      vttui_write_str(BOX_TL);
      if (self->title != mp_const_none) {
        size_t tlen;
        const char *title = mp_obj_str_get_data(self->title, &tlen);
        int inner = w - 2, label_w = (int)tlen + 2;
        if (label_w <= inner) {
          int n1 = (inner - label_w) / 2;
          int n2 = inner - label_w - n1;
          write_repeat_str(BOX_H, 3, n1);
          vttui_write_str("\033[1m");
          vttui_write(" ", 1);
          vttui_write(title, tlen);
          vttui_write(" ", 1);
          vttui_write_str("\033[0m");
          vttui_begin(x + 1 + n1 + label_w, y, self->fg, self->bg, false);
          write_repeat_str(BOX_H, 3, n2);
        } else {
          write_repeat_str(BOX_H, 3, inner);
        }
      } else {
        write_repeat_str(BOX_H, 3, w - 2);
      }
      vttui_write_str(BOX_TR "\033[0m");
      for (int row = 1; row < h - 1; row++) {
        vttui_begin(x, y + row, self->fg, self->bg, false);
        vttui_write_str(BOX_V "\033[0m");
        vttui_begin(x + w - 1, y + row, self->fg, self->bg, false);
        vttui_write_str(BOX_V "\033[0m");
      }
      vttui_begin(x, y + h - 1, self->fg, self->bg, false);
      vttui_write_str(BOX_BL);
      write_repeat_str(BOX_H, 3, w - 2);
      vttui_write_str(BOX_BR "\033[0m");
    }

    // Question text — split on \n, center each line, skip last row (buttons)
    size_t text_len;
    const char *text = mp_obj_str_get_data(self->text, &text_len);
    int text_rows = 0;
    for (size_t i = 0; i < text_len; i++)
      if (text[i] == '\n')
        text_rows++;
    text_rows++; // final line (no trailing newline required)

    int avail_rows = inner_h - 1; // last row is buttons
    int text_y = inner_y + (avail_rows - text_rows) / 2;
    if (text_y < inner_y)
      text_y = inner_y;

    const char *line = text;
    const char *end = text + text_len;
    int row = 0;
    for (const char *p = text; p <= end; p++) {
      if (p == end || *p == '\n') {
        int line_len = (int)(p - line);
        // Center the line within inner_w
        int pad = (inner_w - line_len) / 2;
        if (pad < 0)
          pad = 0;
        render_label_raw(inner_x + pad, text_y + row, line, (size_t)line_len,
                         inner_w - pad, self->fg, self->bg, false, 0);
        row++;
        line = p + 1;
      }
    }
  }

  // Always redraw buttons on selection change or first draw
  render_dialog_buttons(self, inner_x, inner_y, inner_w, inner_h);
  self->last_selected = self->selected;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_dialog_draw_obj, vttui_dialog_draw);

static mp_obj_t vttui_dialog_left(mp_obj_t self_in) {
  vttui_dialog_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (self->selected > 0)
    self->selected = 0;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_dialog_left_obj, vttui_dialog_left);

static mp_obj_t vttui_dialog_right(mp_obj_t self_in) {
  vttui_dialog_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (self->btn2 == mp_const_none)
    return mp_const_none; // nothing to move to
  if (self->selected < 1)
    self->selected = 1;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_dialog_right_obj, vttui_dialog_right);

static const mp_rom_map_elem_t vttui_dialog_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_draw), MP_ROM_PTR(&vttui_dialog_draw_obj)},
    {MP_ROM_QSTR(MP_QSTR_left), MP_ROM_PTR(&vttui_dialog_left_obj)},
    {MP_ROM_QSTR(MP_QSTR_right), MP_ROM_PTR(&vttui_dialog_right_obj)},
};
static MP_DEFINE_CONST_DICT(vttui_dialog_locals_dict,
                            vttui_dialog_locals_dict_table);

static void vttui_dialog_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
  if (dest[0] != MP_OBJ_NULL)
    return;
  vttui_dialog_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (attr == MP_QSTR_selected || attr == MP_QSTR_value) {
    dest[0] = MP_OBJ_NEW_SMALL_INT(self->selected);
    return;
  }
  mp_map_elem_t *elem = mp_map_lookup((mp_map_t *)&vttui_dialog_locals_dict.map,
                                      MP_OBJ_NEW_QSTR(attr), MP_MAP_LOOKUP);
  if (elem) {
    dest[0] = elem->value;
    dest[1] = self_in;
  }
}

MP_DEFINE_CONST_OBJ_TYPE(vttui_dialog_type, MP_QSTR_VTDialog, MP_TYPE_FLAG_NONE,
                         attr, vttui_dialog_attr, locals_dict,
                         &vttui_dialog_locals_dict);

static const mp_arg_t make_dialog_args[] = {
    {MP_QSTR_text, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
    {MP_QSTR_x, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_y, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_width, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_height, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_fg, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_bg, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_btn1,
     MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_OBJ,
     {.u_obj = MP_OBJ_NULL}},
    {MP_QSTR_btn2, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none}},
    {MP_QSTR_sel_fg, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1}},
    {MP_QSTR_sel_bg, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1}},
    {MP_QSTR_selected, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0}},
    {MP_QSTR_title, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none}},
    {MP_QSTR_decorations, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = true}},
};

static vttui_dialog_obj_t *create_dialog(mp_arg_val_t *args, int abs_x,
                                         int abs_y, int width, int height) {
  vttui_dialog_obj_t *dlg = m_new_obj(vttui_dialog_obj_t);
  dlg->base.type = &vttui_dialog_type;
  dlg->x = abs_x;
  dlg->y = abs_y;
  dlg->width = width;
  dlg->height = height;
  dlg->fg = (uint32_t)args[5].u_int;
  dlg->bg = (uint32_t)args[6].u_int;
  dlg->sel_fg = args[9].u_int >= 0 ? (uint32_t)args[9].u_int : dlg->bg;
  dlg->sel_bg = args[10].u_int >= 0 ? (uint32_t)args[10].u_int : dlg->fg;
  dlg->text = args[0].u_obj;
  dlg->btn1 = args[7].u_obj;
  dlg->btn2 = args[8].u_obj;
  dlg->title = args[12].u_obj;
  dlg->selected = (dlg->btn2 == mp_const_none) ? 0 : args[11].u_int;
  dlg->last_selected = -1;
  dlg->decorations = args[13].u_bool;
  return dlg;
}

// ─── VTWindow
// ─────────────────────────────────────────────────────────────────

static void render_window_bg(vttui_window_obj_t *win) {
  for (int row = 0; row < win->inner_h; row++) {
    render_label_raw(win->inner_x, win->inner_y + row, "", 0, win->inner_w,
                     win->fg, win->bg, false, 0);
  }
}

static void render_window_border(vttui_window_obj_t *win) {
  int x = win->x, y = win->y, w = win->width, h = win->height;

  // Top border: ┌──[ title ]──┐
  vttui_begin(x, y, win->fg, win->bg, false);
  vttui_write_str(BOX_TL);
  if (win->title != mp_const_none) {
    size_t tlen;
    const char *title = mp_obj_str_get_data(win->title, &tlen);
    int inner = w - 2, label_w = (int)tlen + 2;
    if (label_w <= inner) {
      int n1 = (inner - label_w) / 2;
      int n2 = inner - label_w - n1;
      write_repeat_str(BOX_H, 3, n1);
      vttui_write_str("\033[1m"); // bold title
      vttui_write(" ", 1);
      vttui_write(title, tlen);
      vttui_write(" ", 1);
      vttui_write_str("\033[0m");
      vttui_begin(x + 1 + n1 + label_w, y, win->fg, win->bg, false);
      write_repeat_str(BOX_H, 3, n2);
    } else {
      write_repeat_str(BOX_H, 3, inner);
    }
  } else {
    write_repeat_str(BOX_H, 3, w - 2);
  }
  vttui_write_str(BOX_TR "\033[0m");

  // Side borders
  for (int row = 1; row < h - 1; row++) {
    vttui_begin(x, y + row, win->fg, win->bg, false);
    vttui_write_str(BOX_V "\033[0m");
    vttui_begin(x + w - 1, y + row, win->fg, win->bg, false);
    vttui_write_str(BOX_V "\033[0m");
  }

  // Bottom border: └──────┘
  vttui_begin(x, y + h - 1, win->fg, win->bg, false);
  vttui_write_str(BOX_BL);
  write_repeat_str(BOX_H, 3, w - 2);
  vttui_write_str(BOX_BR "\033[0m");
}

static mp_obj_t vttui_clear_screen(mp_obj_t self_in) {
  vttui_write_str("\033[2J\033[H");
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_clear_screen_obj, vttui_clear_screen);

static mp_obj_t vttui_enter_altscreen(mp_obj_t self_in) {
  vttui_write_str("\033[?1049h");
  vttui_vttui_obj_t *self = MP_OBJ_TO_PTR(self_in);
  mp_obj_t dest[2];
  mp_load_method_maybe(self->stream_obj, MP_QSTR_repaint_bars, dest);
  if (dest[0] != MP_OBJ_NULL)
    mp_call_method_n_kw(0, 0, dest);
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_enter_altscreen_obj,
                                 vttui_enter_altscreen);

static mp_obj_t vttui_exit_altscreen(mp_obj_t self_in) {
  vttui_write_str("\033[?1049l");
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_exit_altscreen_obj,
                                 vttui_exit_altscreen);

static mp_obj_t vttui_cursor_hide(mp_obj_t self_in) {
  vttui_write_str("\033[?25l");
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_cursor_hide_obj, vttui_cursor_hide);

static mp_obj_t vttui_cursor_show(mp_obj_t self_in) {
  vttui_write_str("\033[?25h");
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_cursor_show_obj, vttui_cursor_show);

static mp_obj_t vttui_window_draw(mp_obj_t self_in) {
  vttui_window_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (!self->dirty)
    return mp_const_false;
  render_window_bg(self);
  if (self->decorations)
    render_window_border(self);
  self->dirty = false;
  return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_window_draw_obj, vttui_window_draw);

static mp_obj_t vttui_window_invalidate(mp_obj_t self_in) {
  ((vttui_window_obj_t *)MP_OBJ_TO_PTR(self_in))->dirty = true;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_window_invalidate_obj,
                                 vttui_window_invalidate);

static mp_obj_t vttui_window_draw_label(size_t n_args, const mp_obj_t *pos_args,
                                        mp_map_t *kw_args) {
  mp_arg_val_t args[MP_ARRAY_SIZE(make_label_args)];
  mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                   MP_ARRAY_SIZE(make_label_args), make_label_args, args);
  vttui_window_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
  uint32_t fg = args[3].u_int < 0 ? 257 : (uint32_t)args[3].u_int;
  uint32_t bg = args[4].u_int < 0 ? 256 : (uint32_t)args[4].u_int;
  vttui_label_obj_t *lbl =
      create_label(args[0].u_obj, args[1].u_int, args[2].u_int, self->inner_x,
                   self->inner_y, self->inner_w, fg, bg, args[5].u_bool,
                   args[7].u_int, parse_align(args[6].u_obj));
  return vttui_label_draw(MP_OBJ_FROM_PTR(lbl));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(vttui_window_draw_label_obj, 4,
                                  vttui_window_draw_label);

static mp_obj_t vttui_window_make_label(size_t n_args, const mp_obj_t *pos_args,
                                        mp_map_t *kw_args) {
  mp_arg_val_t args[MP_ARRAY_SIZE(make_label_args)];
  mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                   MP_ARRAY_SIZE(make_label_args), make_label_args, args);
  vttui_window_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
  uint32_t fg = args[3].u_int < 0 ? 257 : (uint32_t)args[3].u_int;
  uint32_t bg = args[4].u_int < 0 ? 256 : (uint32_t)args[4].u_int;
  return MP_OBJ_FROM_PTR(
      create_label(args[0].u_obj, args[1].u_int, args[2].u_int, self->inner_x,
                   self->inner_y, self->inner_w, fg, bg, args[5].u_bool,
                   args[7].u_int, parse_align(args[6].u_obj)));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(vttui_window_make_label_obj, 4,
                                  vttui_window_make_label);

static mp_obj_t vttui_window_make_list(size_t n_args, const mp_obj_t *pos_args,
                                       mp_map_t *kw_args) {
  mp_arg_val_t args[MP_ARRAY_SIZE(make_list_args)];
  mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                   MP_ARRAY_SIZE(make_list_args), make_list_args, args);
  vttui_window_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
  int rel_x = args[1].u_int, rel_y = args[2].u_int;
  int width = args[3].u_int, height = args[4].u_int;
  int max_w = self->inner_w - rel_x, max_h = self->inner_h - rel_y;
  if (width > max_w)
    width = max_w;
  if (height > max_h)
    height = max_h;
  int abs_x = self->inner_x + rel_x;
  if (parse_align(args[13].u_obj) == 1)
    abs_x = self->inner_x + (self->inner_w - width) / 2;
  return MP_OBJ_FROM_PTR(
      create_list(args, abs_x, self->inner_y + rel_y, width, height));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(vttui_window_make_list_obj, 1,
                                  vttui_window_make_list);

static mp_obj_t vttui_window_make_input(size_t n_args, const mp_obj_t *pos_args,
                                        mp_map_t *kw_args) {
  mp_arg_val_t args[MP_ARRAY_SIZE(make_input_args)];
  mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                   MP_ARRAY_SIZE(make_input_args), make_input_args, args);
  vttui_window_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
  int width = args[3].u_int > 4 ? args[3].u_int : 4;
  int abs_x = (parse_align(args[9].u_obj) == 1)
                  ? self->inner_x + (self->inner_w - width) / 2
                  : self->inner_x + args[1].u_int;
  int abs_y = self->inner_y + args[2].u_int;
  return MP_OBJ_FROM_PTR(create_input(args, abs_x, abs_y));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(vttui_window_make_input_obj, 4,
                                  vttui_window_make_input);

static mp_obj_t vttui_window_make_block(size_t n_args, const mp_obj_t *pos_args,
                                        mp_map_t *kw_args) {
  mp_arg_val_t args[MP_ARRAY_SIZE(make_block_args)];
  mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                   MP_ARRAY_SIZE(make_block_args), make_block_args, args);
  vttui_window_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
  int abs_x = self->inner_x + args[1].u_int;
  int abs_y = self->inner_y + args[2].u_int;
  uint32_t fg = args[3].u_int >= 0 ? (uint32_t)args[3].u_int : self->fg;
  uint32_t bg = args[4].u_int >= 0 ? (uint32_t)args[4].u_int : self->bg;
  int avail = self->inner_w - args[1].u_int;
  int width = args[5].u_int > 0 ? args[5].u_int : (avail > 0 ? avail : 0);
  return MP_OBJ_FROM_PTR(create_block(args, abs_x, abs_y, fg, bg, width,
                                      args[6].u_int, args[7].u_bool,
                                      args[8].u_bool));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(vttui_window_make_block_obj, 3,
                                  vttui_window_make_block);

static mp_obj_t vttui_window_make_dialog(size_t n_args,
                                         const mp_obj_t *pos_args,
                                         mp_map_t *kw_args) {
  mp_arg_val_t args[MP_ARRAY_SIZE(make_dialog_args)];
  mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                   MP_ARRAY_SIZE(make_dialog_args), make_dialog_args, args);
  vttui_window_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
  int rel_x = args[1].u_int, rel_y = args[2].u_int;
  int width = args[3].u_int, height = args[4].u_int;
  int max_w = self->inner_w - rel_x, max_h = self->inner_h - rel_y;
  if (width > max_w)
    width = max_w;
  if (height > max_h)
    height = max_h;
  int abs_x = self->inner_x + rel_x;
  int abs_y = self->inner_y + rel_y;
  return MP_OBJ_FROM_PTR(create_dialog(args, abs_x, abs_y, width, height));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(vttui_window_make_dialog_obj, 1,
                                  vttui_window_make_dialog);

static const mp_rom_map_elem_t vttui_window_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_draw), MP_ROM_PTR(&vttui_window_draw_obj)},
    {MP_ROM_QSTR(MP_QSTR_invalidate), MP_ROM_PTR(&vttui_window_invalidate_obj)},
    {MP_ROM_QSTR(MP_QSTR_cursor_hide), MP_ROM_PTR(&vttui_cursor_hide_obj)},
    {MP_ROM_QSTR(MP_QSTR_cursor_show), MP_ROM_PTR(&vttui_cursor_show_obj)},
    {MP_ROM_QSTR(MP_QSTR_draw_label), MP_ROM_PTR(&vttui_window_draw_label_obj)},
    {MP_ROM_QSTR(MP_QSTR_make_label), MP_ROM_PTR(&vttui_window_make_label_obj)},
    {MP_ROM_QSTR(MP_QSTR_make_list), MP_ROM_PTR(&vttui_window_make_list_obj)},
    {MP_ROM_QSTR(MP_QSTR_make_input), MP_ROM_PTR(&vttui_window_make_input_obj)},
    {MP_ROM_QSTR(MP_QSTR_make_block), MP_ROM_PTR(&vttui_window_make_block_obj)},
    {MP_ROM_QSTR(MP_QSTR_make_dialog),
     MP_ROM_PTR(&vttui_window_make_dialog_obj)},
};
static MP_DEFINE_CONST_DICT(vttui_window_locals_dict,
                            vttui_window_locals_dict_table);

static void vttui_window_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
  if (dest[0] != MP_OBJ_NULL)
    return;
  vttui_window_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (attr == MP_QSTR_inner_w) {
    dest[0] = MP_OBJ_NEW_SMALL_INT(self->inner_w);
    return;
  }
  if (attr == MP_QSTR_inner_h) {
    dest[0] = MP_OBJ_NEW_SMALL_INT(self->inner_h);
    return;
  }
  mp_map_elem_t *elem = mp_map_lookup((mp_map_t *)&vttui_window_locals_dict.map,
                                      MP_OBJ_NEW_QSTR(attr), MP_MAP_LOOKUP);
  if (elem) {
    dest[0] = elem->value;
    dest[1] = self_in;
  }
}

MP_DEFINE_CONST_OBJ_TYPE(vttui_window_type, MP_QSTR_VTWindow, MP_TYPE_FLAG_NONE,
                         attr, vttui_window_attr, locals_dict,
                         &vttui_window_locals_dict);

// ─── VTTUI methods
// ────────────────────────────────────────────────────────────

static mp_obj_t vttui_make_new(const mp_obj_type_t *type, size_t n_args,
                               size_t n_kw, const mp_obj_t *args) {
  mp_arg_check_num(n_args, n_kw, 1, 3, false);

  vttui_vttui_obj_t *self = m_new_obj(vttui_vttui_obj_t);
  self->base.type = type;
  self->stream_obj = args[0];
  self->width = (n_args > 1) ? mp_obj_get_int(args[1]) : 40;
  self->height = (n_args > 2) ? mp_obj_get_int(args[2]) : 16;

  // Set global stream used by draw() and repaint_bars().
  vttui_stream_obj = self->stream_obj;

  return MP_OBJ_FROM_PTR(self);
}

// draw(): flush terminal rows to the LCD, then repaint the bars so they are
// never obscured by terminal rendering.
static mp_obj_t vttui_draw(mp_obj_t self_in) {
  vttui_vttui_obj_t *self = MP_OBJ_TO_PTR(self_in);
  mp_obj_t dest[2];
  mp_load_method_maybe(self->stream_obj, MP_QSTR_draw, dest);
  if (dest[0] != MP_OBJ_NULL)
    mp_call_method_n_kw(0, 0, dest);
  mp_obj_t rb[2];
  mp_load_method_maybe(self->stream_obj, MP_QSTR_repaint_bars, rb);
  if (rb[0] != MP_OBJ_NULL)
    mp_call_method_n_kw(0, 0, rb);
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vttui_draw_obj, vttui_draw);

static mp_obj_t vttui_draw_label(size_t n_args, const mp_obj_t *pos_args,
                                 mp_map_t *kw_args) {
  mp_arg_val_t args[MP_ARRAY_SIZE(make_label_args)];
  mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                   MP_ARRAY_SIZE(make_label_args), make_label_args, args);
  vttui_vttui_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
  uint32_t fg = args[3].u_int < 0 ? 257 : (uint32_t)args[3].u_int;
  uint32_t bg = args[4].u_int < 0 ? 256 : (uint32_t)args[4].u_int;
  vttui_label_obj_t *lbl = create_label(
      args[0].u_obj, args[1].u_int, args[2].u_int, 0, 0, (int)self->width, fg,
      bg, args[5].u_bool, args[7].u_int, parse_align(args[6].u_obj));
  return vttui_label_draw(MP_OBJ_FROM_PTR(lbl));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(vttui_draw_label_obj, 4, vttui_draw_label);

static mp_obj_t vttui_make_label(size_t n_args, const mp_obj_t *pos_args,
                                 mp_map_t *kw_args) {
  mp_arg_val_t args[MP_ARRAY_SIZE(make_label_args)];
  mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                   MP_ARRAY_SIZE(make_label_args), make_label_args, args);
  vttui_vttui_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
  uint32_t fg = args[3].u_int < 0 ? 257 : (uint32_t)args[3].u_int;
  uint32_t bg = args[4].u_int < 0 ? 256 : (uint32_t)args[4].u_int;
  return MP_OBJ_FROM_PTR(create_label(
      args[0].u_obj, args[1].u_int, args[2].u_int, 0, 0, (int)self->width, fg,
      bg, args[5].u_bool, args[7].u_int, parse_align(args[6].u_obj)));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(vttui_make_label_obj, 4, vttui_make_label);

static mp_obj_t vttui_make_list(size_t n_args, const mp_obj_t *pos_args,
                                mp_map_t *kw_args) {
  mp_arg_val_t args[MP_ARRAY_SIZE(make_list_args)];
  mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                   MP_ARRAY_SIZE(make_list_args), make_list_args, args);
  return MP_OBJ_FROM_PTR(create_list(args, args[1].u_int, args[2].u_int,
                                     args[3].u_int, args[4].u_int));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(vttui_make_list_obj, 1, vttui_make_list);

static mp_obj_t vttui_make_input(size_t n_args, const mp_obj_t *pos_args,
                                 mp_map_t *kw_args) {
  mp_arg_val_t args[MP_ARRAY_SIZE(make_input_args)];
  mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                   MP_ARRAY_SIZE(make_input_args), make_input_args, args);
  vttui_vttui_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
  int width = args[3].u_int > 4 ? args[3].u_int : 4;
  int abs_x = (parse_align(args[9].u_obj) == 1) ? ((int)self->width - width) / 2
                                                : args[1].u_int;
  return MP_OBJ_FROM_PTR(create_input(args, abs_x, args[2].u_int));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(vttui_make_input_obj, 4, vttui_make_input);

// make_window(x, y, *, width=0, height=0, title=None, fg=-1, bg=-1,
// decorations=True)
static mp_obj_t vttui_make_block(size_t n_args, const mp_obj_t *pos_args,
                                 mp_map_t *kw_args) {
  mp_arg_val_t args[MP_ARRAY_SIZE(make_block_args)];
  mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                   MP_ARRAY_SIZE(make_block_args), make_block_args, args);
  uint32_t fg = args[3].u_int >= 0 ? (uint32_t)args[3].u_int : 257;
  uint32_t bg = args[4].u_int >= 0 ? (uint32_t)args[4].u_int : 256;
  int width = args[5].u_int;
  return MP_OBJ_FROM_PTR(create_block(args, args[1].u_int, args[2].u_int, fg,
                                      bg, width, args[6].u_int, args[7].u_bool,
                                      args[8].u_bool));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(vttui_make_block_obj, 3, vttui_make_block);

static mp_obj_t vttui_make_dialog(size_t n_args, const mp_obj_t *pos_args,
                                  mp_map_t *kw_args) {
  mp_arg_val_t args[MP_ARRAY_SIZE(make_dialog_args)];
  mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                   MP_ARRAY_SIZE(make_dialog_args), make_dialog_args, args);
  return MP_OBJ_FROM_PTR(create_dialog(args, args[1].u_int, args[2].u_int,
                                       args[3].u_int, args[4].u_int));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(vttui_make_dialog_obj, 1, vttui_make_dialog);

static mp_obj_t vttui_make_window(size_t n_args, const mp_obj_t *pos_args,
                                  mp_map_t *kw_args) {
  static const mp_arg_t allowed_args[] = {
      {MP_QSTR_x, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
      {MP_QSTR_y, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
      {MP_QSTR_width, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0}},
      {MP_QSTR_height, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0}},
      {MP_QSTR_title, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none}},
      {MP_QSTR_fg, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1}},
      {MP_QSTR_bg, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1}},
      {MP_QSTR_decorations, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = true}},
  };
  mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
  mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                   MP_ARRAY_SIZE(allowed_args), allowed_args, args);

  vttui_vttui_obj_t *tui = MP_OBJ_TO_PTR(pos_args[0]);

  vttui_window_obj_t *win = m_new_obj(vttui_window_obj_t);
  win->base.type = &vttui_window_type;
  win->x = args[0].u_int;
  win->y = args[1].u_int;
  win->width = args[2].u_int > 0 ? args[2].u_int : (int)tui->width - win->x;
  win->height = args[3].u_int > 0 ? args[3].u_int : (int)tui->height - win->y;
  win->title = args[4].u_obj;
  win->fg = args[5].u_int < 0 ? 257 : (uint32_t)args[5].u_int;
  win->bg = args[6].u_int < 0 ? 256 : (uint32_t)args[6].u_int;
  win->decorations = args[7].u_bool;
  win->dirty = true;

  if (win->decorations) {
    win->inner_x = win->x + 1;
    win->inner_y = win->y + 1;
    win->inner_w = win->width - 2;
    win->inner_h = win->height - 2;
  } else {
    win->inner_x = win->x;
    win->inner_y = win->y;
    win->inner_w = win->width;
    win->inner_h = win->height;
  }

  return MP_OBJ_FROM_PTR(win);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(vttui_make_window_obj, 1, vttui_make_window);

static const mp_rom_map_elem_t vttui_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_draw), MP_ROM_PTR(&vttui_draw_obj)},
    {MP_ROM_QSTR(MP_QSTR_clear_screen), MP_ROM_PTR(&vttui_clear_screen_obj)},
    {MP_ROM_QSTR(MP_QSTR_cursor_hide), MP_ROM_PTR(&vttui_cursor_hide_obj)},
    {MP_ROM_QSTR(MP_QSTR_cursor_show), MP_ROM_PTR(&vttui_cursor_show_obj)},
    {MP_ROM_QSTR(MP_QSTR_enter_altscreen),
     MP_ROM_PTR(&vttui_enter_altscreen_obj)},
    {MP_ROM_QSTR(MP_QSTR_exit_altscreen),
     MP_ROM_PTR(&vttui_exit_altscreen_obj)},
    {MP_ROM_QSTR(MP_QSTR_draw_label), MP_ROM_PTR(&vttui_draw_label_obj)},
    {MP_ROM_QSTR(MP_QSTR_make_label), MP_ROM_PTR(&vttui_make_label_obj)},
    {MP_ROM_QSTR(MP_QSTR_make_list), MP_ROM_PTR(&vttui_make_list_obj)},
    {MP_ROM_QSTR(MP_QSTR_make_input), MP_ROM_PTR(&vttui_make_input_obj)},
    {MP_ROM_QSTR(MP_QSTR_make_block), MP_ROM_PTR(&vttui_make_block_obj)},
    {MP_ROM_QSTR(MP_QSTR_make_pager), MP_ROM_PTR(&vttui_make_pager_obj)},
    {MP_ROM_QSTR(MP_QSTR_make_dialog), MP_ROM_PTR(&vttui_make_dialog_obj)},
    {MP_ROM_QSTR(MP_QSTR_make_window), MP_ROM_PTR(&vttui_make_window_obj)},
};
static MP_DEFINE_CONST_DICT(vttui_locals_dict, vttui_locals_dict_table);

static void vttui_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
  vttui_vttui_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (dest[0] == MP_OBJ_SENTINEL) {
    // Store
    if (attr == MP_QSTR_width || attr == MP_QSTR_cols) {
      self->width = (uint16_t)mp_obj_get_int(dest[1]);
      dest[0] = MP_OBJ_NULL;
    } else if (attr == MP_QSTR_height || attr == MP_QSTR_rows) {
      self->height = (uint16_t)mp_obj_get_int(dest[1]);
      dest[0] = MP_OBJ_NULL;
    }
    return;
  }
  if (dest[0] != MP_OBJ_NULL)
    return;
  // Load
  if (attr == MP_QSTR_width || attr == MP_QSTR_cols) {
    dest[0] = MP_OBJ_NEW_SMALL_INT(self->width);
    return;
  }
  if (attr == MP_QSTR_height || attr == MP_QSTR_rows) {
    dest[0] = MP_OBJ_NEW_SMALL_INT(self->height);
    return;
  }
  mp_map_elem_t *elem = mp_map_lookup((mp_map_t *)&vttui_locals_dict.map,
                                      MP_OBJ_NEW_QSTR(attr), MP_MAP_LOOKUP);
  if (elem) {
    dest[0] = elem->value;
    dest[1] = self_in;
  }
}

MP_DEFINE_CONST_OBJ_TYPE(vttui_type, MP_QSTR_VTTU, MP_TYPE_FLAG_NONE, make_new,
                         vttui_make_new, attr, vttui_attr, locals_dict,
                         &vttui_locals_dict);

// ─── Module
// ───────────────────────────────────────────────────────────────────

static const mp_rom_map_elem_t modtui_module_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_modtui)},
    {MP_ROM_QSTR(MP_QSTR_VTTUI), MP_ROM_PTR(&vttui_type)},
};
static MP_DEFINE_CONST_DICT(modtui_module_globals, modtui_module_globals_table);

const mp_obj_module_t modtui_user_cmodule = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&modtui_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_modtui, modtui_user_cmodule);
