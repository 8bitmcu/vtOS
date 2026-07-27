/*
 * MicroPython ANSI Terminal Wrapper
 * Copyright (c) 2026 Vincent (8bitmcu)
 * * Based on the st (Suckless Terminal) engine.
 * Original code (c) st engineers.
 * License: MIT
 */

#ifndef FB_H
#define FB_H

#include "st.h" /* To get the Glyph and Rune types */
#include "st7789.h"
#include "win.h" /* To ensure we match original X11 signatures */
#include <stdint.h>

typedef struct _vt_VT_obj_t {
  mp_obj_base_t base;
  st7789_ST7789_obj_t *display_drv;
  mp_obj_module_t *font;
  // Optional, independent of `font`: a supplemental double-width glyph
  // source (WIDE_FONT/WIDE_FIRST/WIDE_COUNT/WIDE_WIDTH -- see fb.c's
  // xdrawline()) for ATTR_WIDE codepoints, e.g. an icon font like Siji
  // paired with whichever text font is active. NULL if unset -- ATTR_WIDE
  // cells then just render narrow, same as any other missing glyph.
  // Set via VT.set_icon_font(), independent of the main font/layout.
  mp_obj_module_t *icon_font;
} vt_VT_obj_t;

// Persistent line for the status bar -- defined once in fb.c. Was
// `static` right here in the header, which (since fb.c and modvt.c
// both include it) meant each translation unit silently got its own
// disconnected copy: modvt.c's top_bar_invalidate()/
// bottom_bar_invalidate() were clearing a phantom array draw_bar_ansi()'s
// dirty-check (fb.c) never actually read, so invalidate() has been a
// no-op. A single real definition, shared via extern, is also what lets
// modvt.c's render_row() binding read the bars' actual Glyphs.
extern Glyph top_line_now[256];
extern Glyph top_line_last[256];

extern Glyph bot_line_now[256];  // Buffer for bottom bar
extern Glyph bot_line_last[256]; // Back-buffer for bottom bar

extern vt_VT_obj_t *current_vt_obj;

static unsigned int wmode = MODE_VISIBLE;

/* Window/UI operations */
void xsettitle(char *p);
void xseticontitle(char *p);
void xsetsel(char *p);
void xclipcopy(void);

/* Configuration/Settings */
extern int allowwindowops;

/* Color and Graphics */
int xsetcolorname(int n, const char *s);
void xloadcols(void);

/* Drawing/Cursor operations */
void xdrawcursor(int cx, int cy, Glyph g, int ox, int oy, Glyph og);
void xdrawglyph(Glyph g, int x, int y);

/* Mode and Input stubs */
void xsetpointermotion(int val);
int xsetcursor(int cursor);
void xbell(void);
int xgetcolor(int x, unsigned char *r, unsigned char *g, unsigned char *b);

/* Global config variables used by st_term.c */
extern int allowaltscreen;
extern char *vtiden;
extern wchar_t *worddelimiters;
extern unsigned int defaultcs;

void draw_bar_ansi(const char *text, size_t len, int bar_type);
void repaint_bars(void);
void fill_bottom_margin(void);

// Result of render_row_rgb565() -- see fb.c. x_start/y_start/x_end/
// f_height are the same coordinates xdrawline() would pass to
// set_window() for this row; window_width/pixel_count describe what
// actually landed in the caller's buffer. pixel_count == 0 means the row
// was off-screen or the buffer was missing/too small -- none of the
// other fields are meaningful in that case.
typedef struct {
  uint16_t x_start;
  uint16_t y_start;
  uint16_t x_end;
  uint16_t window_width;
  uint8_t f_height;
  size_t pixel_count;
} row_render_t;

// Renders Glyphs -> RGB565 pixels for one text row (or bar, via the same
// _y1 == -1/-2 convention as xdrawline()/draw_bar_ansi()) into a caller-
// supplied buffer, with no SPI/display side effects at all. out_cap is in
// uint16_t pixels, not bytes. See fb.c for the full explanation -- this is
// what lets something other than the physical display (e.g. a VNC server)
// pull the same pixels a redraw would produce.
row_render_t render_row_rgb565(Line ln, int _x1, int _y1, int _x2,
                               uint16_t *out_buf, size_t out_cap);

#endif /* FB_H */
