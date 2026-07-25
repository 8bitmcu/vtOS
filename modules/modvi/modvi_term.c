/*
 * modvi_term.c -- replaces neatvi/term.c's terminal I/O with the KVM
 * stream + status-bar-reported size this project's shell environment
 * provides, instead of a real POSIX tty. Keeps every public function
 * neatvi's other files call (see vi.h's "managing the terminal" section)
 * with identical signatures, so nothing outside this file needs to
 * change to call them.
 * Copyright (c) 2026 8bitmcu
 * License: MIT
 *
 * Most of this is copied verbatim from the original term.c: term_str/
 * term_chr/term_kill/term_room/term_pos/term_rows/term_cols/term_rowx/
 * term_window/term_seqattr/term_seqkill/term_push/term_cmd are pure
 * string-buffer logic with no POSIX dependency at all. Only term_init/
 * term_done/term_suspend/term_commit/term_read are actually rewritten.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "py/mphal.h"
#include "py/runtime.h"

#include "vi.h"
#include "modvi.h"

static struct sbuf *term_sbuf; /* output buffer if not NULL */
static int rows, cols;         /* number of terminal rows and columns */
static int win_top, win_rows;  /* active window rows */
static int win_left, win_cols; /* active window columns */

void term_init(void) {
  // No real POSIX tty here -- width/height come from the shell
  // environment's env.cols/env.rows (see modvi.c's constructor),
  // already stashed on current_modvi_instance by the time term_init()
  // runs (modvi_main() sets it up before calling into neatvi's own
  // startup, same ordering as toybox's vi_main()).
  rows = current_modvi_instance ? current_modvi_instance->height : 25;
  cols = current_modvi_instance ? current_modvi_instance->width : 80;
  rows = rows ? rows : 25;
  cols = cols ? cols : 80;
  term_sbuf = sbuf_make();
  term_str("\33[m");
  term_window(win_top, win_rows > 0 ? win_rows : rows);
}

void term_window(int row, int cnt) {
  char cmd[64];
  win_top = row;
  win_rows = cnt;
  win_left = 0;
  win_cols = cols;
  if (row == 0 && win_rows == rows) {
    term_str("\33[r");
  } else {
    sprintf(cmd, "\33[%d;%dr", win_top + 1, win_top + win_rows);
    term_str(cmd);
  }
}

void term_done(void) {
  term_str("\33[r");
  term_pos(rows - 1, 0);
  term_kill();
  term_commit();
  sbuf_free(term_sbuf);
  term_sbuf = NULL;
  // No real tty raw-mode state to restore -- see term_init()'s comment.
}

void term_suspend(void) {
  // Ctrl-Z / job control has no meaning on this single-process embedded
  // target -- there's no shell to suspend to, and no SIGCONT will ever
  // arrive. A real kill(0, SIGSTOP) here would just freeze the whole
  // device with no way to resume it, so this is a safe no-op instead of
  // the original term_done()+kill(SIGSTOP)+term_init() sequence.
  ex_show("suspend not supported");
}

void term_clear(void) {
  // Declared in vi.h but never actually called anywhere in neatvi --
  // kept as a harmless no-op for completeness.
}

static void write_fully(const char *buf, long sz) {
  mp_hal_stdout_tx_strn(buf, sz);
}

void term_commit(void) {
  write_fully(sbuf_buf(term_sbuf), sbuf_len(term_sbuf));
  sbuf_cut(term_sbuf, 0);
}

void term_str(char *s) { sbuf_str(term_sbuf, s); }

void term_chr(int ch) {
  char s[4] = {ch};
  term_str(s);
}

void term_kill(void) { term_str("\33[K"); }

void term_room(int n) {
  char cmd[16];
  if (n < 0)
    sprintf(cmd, "\33[%dM", -n);
  if (n > 0)
    sprintf(cmd, "\33[%dL", n);
  if (n)
    term_str(cmd);
}

void term_pos(int r, int c) {
  char buf[32];
  if (c < 0)
    c = 0;
  if (c >= term_cols())
    c = win_cols - 1;
  if (r < 0)
    sprintf(buf, "\33[%dG", win_left + c + 1);
  else
    sprintf(buf, "\33[%d;%dH", win_top + r + 1, win_left + c + 1);
  term_str(buf);
}

int term_rowx(void) { return rows; }
int term_rows(void) { return win_rows; }
int term_cols(void) { return win_cols; }

static EXT_RAM_BSS_ATTR char ibuf[4096]; /* input character buffer */
static EXT_RAM_BSS_ATTR char icmd[4096]; /* read after the last term_cmd() */
static int ibuf_pos, ibuf_cnt; /* ibuf[] position and length */
static int icmd_pos;          /* icmd[] position */

void term_push(char *s, int n) {
  int cur = ibuf_cnt - ibuf_pos;
  n = MIN(n, (int)sizeof(ibuf) - cur);
  memmove(ibuf + n, ibuf + ibuf_pos, cur);
  memcpy(ibuf, s, n);
  ibuf_pos = 0;
  ibuf_cnt = cur + n;
}

char *term_cmd(int *n) {
  *n = icmd_pos;
  icmd_pos = 0;
  return icmd;
}

// Reads one byte from the KVM stream, polling with a yield between
// attempts (mp_handle_pending()/mp_hal_delay_ms()) so MicroPython's
// scheduler and the watchdog keep running while "blocked" waiting for a
// keypress -- same mechanism modules/vi/vi.c's scan_key_getsize() already
// proved out on this exact hardware. timeout_ms < 0 waits indefinitely;
// otherwise returns -1 if nothing arrives within that window.
static int term_fetch_byte(int timeout_ms) {
  uint32_t start = mp_hal_ticks_ms();
  for (;;) {
    uint8_t byte;
    int errcode;
    modvi_vi_obj_t *vi = current_modvi_instance;
    mp_uint_t n = vi->stream_p->read(vi->stream_obj, &byte, 1, &errcode);
    if (n != (mp_uint_t)-1 && n > 0) {
      // The keyboard driver (modules/tdeck_kbd/tdeck_kbd.c) always sends
      // '\r' (0x0D) for Enter, never '\n' -- confirmed directly, it
      // normalizes both possible raw codes to 0x0D before this stream
      // ever sees a byte. neatvi's own key dispatch (throughout vi.c/
      // ex.c/led.c) checks for '\n' to mean Enter/confirm, matching a
      // real terminal's ICRNL-translated input -- but ICRNL is real
      // termios state with no effect here (no real tty). Translating
      // here, once, at the single point every keystroke funnels
      // through, is equivalent to what ICRNL would normally do, without
      // needing to hunt down and patch every '\n' check throughout the
      // vendored source. Scoped to interactive input only -- file
      // content loaded via lbuf_rd()/vfs_read() never passes through
      // here, so a real '\r' byte inside a file's own text is untouched.
      if (byte == '\r') {
        byte = '\n';
      }
      return byte;
    }
    if (timeout_ms >= 0 &&
        (mp_hal_ticks_ms() - start) > (uint32_t)timeout_ms) {
      return -1;
    }
    mp_handle_pending(false);
    mp_hal_delay_ms(30);
  }
}

int term_read(int buffered) {
  int c;
  if (ibuf_pos >= ibuf_cnt) {
    // buffered=0 (a normal keypress read): wait indefinitely.
    // buffered=1: this is neatvi's vi_readiseq() asking "is there
    // already a next byte queued up" to tell a standalone Escape
    // keypress apart from the start of an arrow-key/function-key
    // escape sequence. The original term.c answered that instantly by
    // checking whether a single real read(2) call already delivered
    // more than one byte (true terminals deliver a fast escape
    // sequence's bytes together in one read()). A byte-at-a-time
    // non-blocking KVM read can't batch that way, so this gives it a
    // short grace window instead of returning -1 immediately -- same
    // 30ms value modules/vi/vi.c's own escape-sequence handling uses.
    int b = term_fetch_byte(buffered ? 30 : -1);
    if (b < 0) {
      return -1;
    }
    ibuf[0] = (char)b;
    ibuf_cnt = 1;
    ibuf_pos = 0;
  }
  c = ibuf_pos < ibuf_cnt ? (unsigned char)ibuf[ibuf_pos++] : -1;
  if (icmd_pos < (int)sizeof(icmd)) {
    icmd[icmd_pos++] = c;
  }
  return c;
}

char *term_seqattr(int att, int old) {
  static char buf[128];
  char *s = buf;
  int fg = SYN_FG(att);
  int bg = SYN_BG(att);
  if (att == old)
    return "";
  s += sprintf(s, "\33[");
  if (att & SYN_BD)
    s += sprintf(s, ";1");
  if (att & SYN_IT)
    s += sprintf(s, ";3");
  if (att & SYN_RV)
    s += sprintf(s, ";7");
  if (SYN_FGSET(att)) {
    if (att & SYN_FGTC) {
      int r, g, b;
      syn_tcget(fg & 0xff, &r, &g, &b);
      s += sprintf(s, ";38;2;%d;%d;%d", r, g, b);
    } else if ((fg & 0xff) < 8)
      s += sprintf(s, ";%d", 30 + (fg & 0xff));
    else
      s += sprintf(s, ";38;5;%d", (fg & 0xff));
  }
  if (SYN_BGSET(att)) {
    if (att & SYN_BGTC) {
      int r, g, b;
      syn_tcget(bg & 0xff, &r, &g, &b);
      s += sprintf(s, ";48;2;%d;%d;%d", r, g, b);
    } else if ((bg & 0xff) < 8)
      s += sprintf(s, ";%d", 40 + (bg & 0xff));
    else
      s += sprintf(s, ";48;5;%d", (bg & 0xff));
  }
  s += sprintf(s, "m");
  return buf;
}

char *term_seqkill(void) { return "\33[K"; }
