/*
 * MicroPython ANSI Terminal Wrapper
 * Copyright (c) 2026 8bitmcu
 * * Based on the st (Suckless Terminal) engine.
 * Original code (c) st engineers.
 * License: MIT
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "py/mphal.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "esp_system.h"

#include "fb.h"
#include "mpfile.h"
#include "st.h"
#include "st7789.h"

extern int twrite(const char *buf, int buflen, int show_ctrl);

// Object Structure ---

extern const mp_obj_type_t st7789_ST7789_type;
vt_VT_obj_t *current_vt_obj = NULL;

// Forward declaration
const mp_obj_type_t vt_VT_type;

static void vt_internal_write(const char *buf, size_t size) {
  twrite(buf, size, 0);
  if (size > 64)
    vTaskDelay(1);
}

static mp_uint_t vt_read(mp_obj_t self_in, void *buf, mp_uint_t size,
                         int *errcode) {
  *errcode = MP_EAGAIN;
  return MP_STREAM_ERROR;
}

static mp_uint_t vt_write(mp_obj_t self_in, const void *buf, mp_uint_t size,
                          int *errcode) {
  vt_internal_write((const char *)buf, size);
  return size;
}

static mp_uint_t vt_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg,
                          int *errcode) {
  if (request == MP_STREAM_POLL) {
    // We are always writable, but never readable (for now)
    return MP_STREAM_POLL_WR;
  } else if (request == MP_STREAM_CLOSE) {
    return 0;
  }

  *errcode = MP_EINVAL;
  return MP_STREAM_ERROR;
}

static mp_obj_t vt_VT_write(mp_obj_t self_in, mp_obj_t arg) {
  mp_buffer_info_t bufinfo;

  // Check if the object is a bytearray or bytes first
  if (mp_get_buffer(arg, &bufinfo, MP_BUFFER_READ)) {
    vt_internal_write((const char *)bufinfo.buf, bufinfo.len);
  } else {
    // Fallback to string handling
    size_t len;
    const char *data = mp_obj_str_get_data(arg, &len);
    vt_internal_write(data, len);
  }

  return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(vt_VT_write_obj, vt_VT_write);

static mp_obj_t vt_VT_draw(mp_obj_t self_in) {
  draw();
  vTaskDelay(1);
  return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(vt_VT_draw_obj, vt_VT_draw);

static mp_obj_t vt_VT_scrollup(mp_obj_t self_in) {
  kscrollup(&(const Arg){.i = -1});
  return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(vt_VT_scrollup_obj, vt_VT_scrollup);

static mp_obj_t vt_VT_scrolldown(mp_obj_t self_in) {
  kscrolldown(&(const Arg){.i = -1});
  return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(vt_VT_scrolldown_obj, vt_VT_scrolldown);

static mp_obj_t vt_vt_top_offset(mp_obj_t self_in, mp_obj_t offset_obj) {
  int offset = mp_obj_get_int(offset_obj);

  // Update the offset in the terminal engine
  term.top_offset = offset;

  // Mark lines dirty so they move to the new position on next draw()
  for (int i = 0; i < term.row; i++) {
    term.dirty[i] = 1;
  }

  // Called after update_layout()/tresize() on both initial boot and
  // every runtime font change (see main.py's update_font()), so
  // term.row is already current here -- this is the one place term.row
  // and term.top_offset are both guaranteed fresh together.
  fill_bottom_margin();

  return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(vt_vt_top_offset_obj, vt_vt_top_offset);

// Top Bar Bridge
// Optimized Top Bar Bridge - Accepts String, Bytes, or Bytearray
static mp_obj_t vt_vt_top_bar(mp_obj_t self_in, mp_obj_t str_obj) {
  mp_buffer_info_t bufinfo;
  if (mp_get_buffer(str_obj, &bufinfo, MP_BUFFER_READ)) {
    draw_bar_ansi((const char *)bufinfo.buf, bufinfo.len, -1);
  } else {
    size_t len;
    const char *txt = mp_obj_str_get_data(str_obj, &len);
    draw_bar_ansi(txt, len, -1);
  }
  return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(vt_vt_top_bar_obj, vt_vt_top_bar);

// Bottom Bar Bridge
static mp_obj_t vt_vt_bottom_bar(mp_obj_t self_in, mp_obj_t str_obj) {
  mp_buffer_info_t bufinfo;

  // Check if the object supports the buffer protocol (bytes, bytearray, etc.)
  if (mp_get_buffer(str_obj, &bufinfo, MP_BUFFER_READ)) {
    // Use raw bytes directly from the bytearray/bytes object
    draw_bar_ansi((const char *)bufinfo.buf, bufinfo.len, -2);
  } else {
    // Fallback for strings
    size_t len;
    const char *txt = mp_obj_str_get_data(str_obj, &len);
    draw_bar_ansi(txt, len, -2);
  }

  return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(vt_vt_bottom_bar_obj, vt_vt_bottom_bar);

// Invalidate the Top Bar cache
static mp_obj_t vt_vt_top_bar_invalidate(mp_obj_t self_in) {
  memset(top_line_last, 0, sizeof(top_line_last));
  return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(vt_vt_top_bar_invalidate_obj,
                          vt_vt_top_bar_invalidate);

// Invalidate the Bottom Bar cache
static mp_obj_t vt_vt_bottom_bar_invalidate(mp_obj_t self_in) {
  memset(bot_line_last, 0, sizeof(bot_line_last));
  return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(vt_vt_bottom_bar_invalidate_obj,
                          vt_vt_bottom_bar_invalidate);

// Force repaint of bars to LCD regardless of dirty state
static mp_obj_t vt_vt_repaint_bars(mp_obj_t self_in) {
  repaint_bars();
  return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(vt_vt_repaint_bars_obj, vt_vt_repaint_bars);

static const mp_stream_p_t vt_stream_p = {
    .read = vt_read,
    .write = vt_write,
    .ioctl = vt_ioctl,
    .is_text = true,
};

//  Class Definition ---

// Constructor: modvt.VT()
// Constructor: modvt.VT(tft, env)
static mp_obj_t vt_VT_make_new(const mp_obj_type_t *type, size_t n_args,
                               size_t n_kw, const mp_obj_t *args) {
  // Expect exactly 2 positional arguments: display driver and environment
  // object
  mp_arg_check_num(n_args, n_kw, 2, 2, false);

  vt_VT_obj_t *self = m_new_obj(vt_VT_obj_t);
  self->base.type = &vt_VT_type;

  if (!mp_obj_is_type(args[0], &st7789_ST7789_type)) {
    mp_raise_TypeError(MP_ERROR_TEXT("Arg 1 must be ST7789 object"));
  }
  self->display_drv = (st7789_ST7789_obj_t *)MP_OBJ_TO_PTR(args[0]);

  // Extract the Python 'env' object passed as Arg 2
  mp_obj_t env_obj = args[1];

  mp_obj_t cols_obj = mp_load_attr(env_obj, MP_QSTR_cols);
  int cols = mp_obj_get_int(cols_obj);

  mp_obj_t rows_obj = mp_load_attr(env_obj, MP_QSTR_rows);
  int rows = mp_obj_get_int(rows_obj);

  mp_obj_t font_obj = mp_load_attr(env_obj, MP_QSTR_font);
  self->font = (mp_obj_module_t *)MP_OBJ_TO_PTR(font_obj);
  self->icon_font = NULL; // no iconic font paired in by default

  // Initialize the terminal grid engine with the extracted dimensions
  tnew(cols, rows);

  current_vt_obj = self;

  return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t vt_VT_update_layout(size_t n_args, const mp_obj_t *args) {
  // args[0] is 'self'
  vt_VT_obj_t *self = MP_OBJ_TO_PTR(args[0]);

  mp_obj_t font_obj = args[1];
  self->font = (mp_obj_module_t *)MP_OBJ_TO_PTR(font_obj);

  int cols = mp_obj_get_int(args[2]);
  int rows = mp_obj_get_int(args[3]);

  tresize(cols, rows);

  return mp_const_none;
}
// Defines a variable-argument function requiring exactly 4 arguments (self + 3
// inputs)
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(vt_VT_update_layout_obj, 4, 4,
                                    vt_VT_update_layout);

// VT.set_icon_font(font_module_or_none) -- independent of the main font/
// layout (see update_layout()). Pass None to clear it. Doesn't touch
// cols/rows: an iconic font only ever supplies WIDE_FONT glyph data for
// ATTR_WIDE codepoints (see fb.c's xdrawline()), never the main grid.
static mp_obj_t vt_VT_set_icon_font(mp_obj_t self_in, mp_obj_t font_obj) {
  vt_VT_obj_t *self = MP_OBJ_TO_PTR(self_in);
  self->icon_font = (font_obj == mp_const_none)
                        ? NULL
                        : (mp_obj_module_t *)MP_OBJ_TO_PTR(font_obj);
  return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(vt_VT_set_icon_font_obj, vt_VT_set_icon_font);

// modvt.render_row(y, buffer) -> int bytes written (0 if the row is out of
// range or buffer is missing/too small). y follows the same convention
// as draw_bar_ansi()'s bar_type: -1 = top bar, -2 = bottom bar, 0..rows-1
// = a terminal text row (via TLINE(), so scrollback-aware like the
// physical display). buffer must support the buffer protocol (a
// bytearray) and be at least width * font_height * 2 bytes -- same
// sizing rule as ST7789's buffer_size.
//
// This is render_row_rgb565() (fb.c) exposed to Python with no SPI/
// display side effects at all -- for pulling the exact same pixels a
// screen redraw would produce, without touching the physical screen.
// e.g. a VNC server walking term.dirty[] for changed rows.
static mp_obj_t vt_VT_render_row(mp_obj_t self_in, mp_obj_t y_obj,
                                 mp_obj_t buf_obj) {
  int y = mp_obj_get_int(y_obj);

  Line ln;
  if (y == -1) {
    ln = top_line_now;
  } else if (y == -2) {
    ln = bot_line_now;
  } else if (y >= 0 && y < term.row) {
    ln = TLINE(y);
  } else {
    return mp_obj_new_int(0);
  }

  mp_buffer_info_t bufinfo;
  mp_get_buffer_raise(buf_obj, &bufinfo, MP_BUFFER_WRITE);

  row_render_t r = render_row_rgb565(ln, 0, y, term.col,
                                     (uint16_t *)bufinfo.buf,
                                     bufinfo.len / sizeof(uint16_t));
  return mp_obj_new_int((mp_int_t)(r.pixel_count * 2));
}
MP_DEFINE_CONST_FUN_OBJ_3(vt_VT_render_row_obj, vt_VT_render_row);

// Locals Dict (Methods attached to the object)
// Even if empty, it's good practice to define it to avoid segfaults on dir(obj)
static const mp_rom_map_elem_t vtinal_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_update_layout), MP_ROM_PTR(&vt_VT_update_layout_obj)},
    {MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&vt_VT_write_obj)},
    {MP_ROM_QSTR(MP_QSTR_draw), MP_ROM_PTR(&vt_VT_draw_obj)},
    {MP_ROM_QSTR(MP_QSTR_scrollup), MP_ROM_PTR(&vt_VT_scrollup_obj)},
    {MP_ROM_QSTR(MP_QSTR_scrolldown), MP_ROM_PTR(&vt_VT_scrolldown_obj)},
    {MP_ROM_QSTR(MP_QSTR_top_offset), MP_ROM_PTR(&vt_vt_top_offset_obj)},
    {MP_ROM_QSTR(MP_QSTR_top_bar), MP_ROM_PTR(&vt_vt_top_bar_obj)},
    {MP_ROM_QSTR(MP_QSTR_bottom_bar), MP_ROM_PTR(&vt_vt_bottom_bar_obj)},
    {MP_ROM_QSTR(MP_QSTR_top_bar_invalidate),
     MP_ROM_PTR(&vt_vt_top_bar_invalidate_obj)},
    {MP_ROM_QSTR(MP_QSTR_bottom_bar_invalidate),
     MP_ROM_PTR(&vt_vt_bottom_bar_invalidate_obj)},
    {MP_ROM_QSTR(MP_QSTR_repaint_bars), MP_ROM_PTR(&vt_vt_repaint_bars_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_icon_font), MP_ROM_PTR(&vt_VT_set_icon_font_obj)},
    {MP_ROM_QSTR(MP_QSTR_render_row), MP_ROM_PTR(&vt_VT_render_row_obj)},
};

static MP_DEFINE_CONST_DICT(vt_VT_locals_dict, vtinal_locals_dict_table);

static void vt_VT_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
  if (dest[0] != MP_OBJ_NULL)
    return;
  if (attr == MP_QSTR_cols) {
    dest[0] = MP_OBJ_NEW_SMALL_INT(term.col);
    return;
  }
  if (attr == MP_QSTR_rows) {
    dest[0] = MP_OBJ_NEW_SMALL_INT(term.row);
    return;
  }
  mp_map_elem_t *elem = mp_map_lookup((mp_map_t *)&vt_VT_locals_dict.map,
                                      MP_OBJ_NEW_QSTR(attr), MP_MAP_LOOKUP);
  if (elem) {
    dest[0] = elem->value;
    dest[1] = self_in;
  }
}

// Type Definition
MP_DEFINE_CONST_OBJ_TYPE(vt_VT_type, MP_QSTR_VT, MP_TYPE_FLAG_NONE, make_new,
                         vt_VT_make_new, protocol, &vt_stream_p, attr,
                         vt_VT_attr, locals_dict, &vt_VT_locals_dict);

//  Module Definition

// machine.reset_cause() buckets ESP-IDF's ESP_RST_INT_WDT/ESP_RST_TASK_WDT/
// ESP_RST_WDT (the last being the RTC-level "super watchdog", which
// doesn't go through the software panic handler at all) into a single
// WDT_RESET. Exposes the raw esp_reset_reason_t int instead, for
// distinguishing which watchdog (if any) actually caused a reset.
static mp_obj_t vt_reset_reason(void) {
  return mp_obj_new_int((mp_int_t)esp_reset_reason());
}
static MP_DEFINE_CONST_FUN_OBJ_0(vt_reset_reason_obj, vt_reset_reason);

static const mp_rom_map_elem_t modvt_module_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_modvt)},
    {MP_ROM_QSTR(MP_QSTR_VT), MP_ROM_PTR(&vt_VT_type)},
    {MP_ROM_QSTR(MP_QSTR_reset_reason), MP_ROM_PTR(&vt_reset_reason_obj)},
};

static MP_DEFINE_CONST_DICT(modvt_module_globals, modvt_module_globals_table);

const mp_obj_module_t modvt_module = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&modvt_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_modvt, modvt_module);
