/*
 * modzm_compat.h -- force-included (via -include, see micropython.cmake)
 * into frotz's own vendored .c files only. Copyright (c) 2026 8bitmcu.
 * License: MIT
 *
 * Two things live here, merged from what used to be two separate
 * files (this one plus frotz/frotz_utils.h) since both exist for the
 * same reason: bridging frotz's plain-libc/POSIX assumptions onto this
 * MicroPython/FreeRTOS target.
 *
 * 1. malloc/realloc/free -> PSRAM-preferring equivalents. Same class of
 * problem already found and fixed this project for modvi's vendored
 * neatvi (see modules/modvi/modvi_compat.h) and for wolfSSL/wolfSSH
 * (modules/modssh/wolfssl_shim/psram_alloc.c): frotz has no central
 * allocator wrapper either -- zmalloc/zrealloc/zfree (frotz.h) are
 * themselves just bare malloc/realloc/free on this platform (the
 * MSDOS_16BIT branch that redirects to halloc/farrealloc doesn't apply
 * here), and plenty of other sites (blorb resource loading, filename
 * buffers, the undo mechanism's prev_zmp/undo_diff buffers sized to the
 * story's dynamic memory, screen buffers) call malloc/realloc/free
 * directly, bypassing those macros entirely. None of it had any PSRAM
 * awareness before this.
 *
 * Object-like macros, not function-like ones -- see modvi_compat.h's
 * own header comment for exactly why function-like substitution breaks
 * the moment a vendored file's own #include <stdlib.h> reaches malloc/
 * realloc/free's *declarations*.
 *
 * 2. A VFS-aware stdio bridge (modzm_fopen/fread/fseek/ftell/fclose),
 * KVM-stream-aware character I/O (xgetchar/modzm_putchar), an RTOS
 * yield that also pumps pending Python callbacks (modzm_yield), and
 * modzm_printf/modzm_basename. None of this is bare-malloc-related, but
 * it's scoped to the exact same set of vendored frotz\**.c translation
 * units (never modzm.c, which uses MicroPython's own m_new_obj/stream
 * APIs directly), so it lives in the same force-included header rather
 * than a third file.
 *
 * xgetchar/modzm_putchar are marked inline here (frotz_utils.h had them
 * as plain static) since force-including this header into every
 * vendored source -- not just the one file that calls each -- would
 * otherwise make them defined-but-unused in every other translation
 * unit, tripping -Wunused-function.
 */

#ifndef MODZM_COMPAT_H
#define MODZM_COMPAT_H

#include <stddef.h>

void *modzm_malloc(size_t n);
void *modzm_realloc(void *p, size_t n);
void modzm_free(void *p);

#define malloc modzm_malloc
#define realloc modzm_realloc
#define free modzm_free

#include "modzm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "py/misc.h"
#include "py/mphal.h"
#include "py/nlr.h"
#include "py/runtime.h"
#include "py/stream.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern modzm_zm_obj_t *current_frotz_instance;

static inline char *modzm_basename(char *path) {
  char *base = strrchr(path, '/');
  return base ? base + 1 : path;
}

/* Yield to the RTOS, running any pending Python callbacks (e.g. term.draw())
 * and then unconditionally handing the scheduler one tick via vTaskDelay(1).
 *
 * mp_hal_delay_ms(ms) alone is not sufficient: it exits as soon as the elapsed
 * time exceeds `ms`, which means it skips vTaskDelay() entirely when
 * mp_handle_pending() callbacks (like term.draw() at ~24 ms) take longer than
 * the requested delay.  The explicit vTaskDelay(1) below guarantees the RTOS
 * idle task always gets CPU time and any active watchdog timers are fed,
 * regardless of how long the callbacks took.
 *
 * Any Python exception raised by a callback is caught by the nlr_push/pop and
 * silently discarded — frotz's C call stack cannot handle Python exceptions. */
static inline void modzm_yield(mp_uint_t ms) {
  nlr_buf_t nlr;
  if (nlr_push(&nlr) == 0) {
    mp_hal_delay_ms(ms);
    nlr_pop();
  }

  vTaskDelay(1);
}

static inline int xgetchar(void) {
  while (1) {
    uint8_t byte;
    int errcode;

    mp_uint_t n = current_frotz_instance->stream_p->read(
        current_frotz_instance->stream_obj, &byte, 1, &errcode);

    if (n != (mp_uint_t)-1 && n > 0) {
      if (byte == '\r') {
        // Enter: echo a newline and return \n for frotz's dumb_getline().
        mp_hal_stdout_tx_strn("\r\n", 2);
        return '\n';
      }
      // Echo printable characters to the display as they are typed.
      if (byte >= 32 && byte < 127) {
        mp_hal_stdout_tx_strn((const char *)&byte, 1);
      }
      return (int)byte;
    }

    // No key available: yield to the RTOS so the WDT gets fed.
    modzm_yield(10);
  }
}

static inline void modzm_putchar(char c) {
  // Write exactly 1 character to the active MicroPython stdout stream
  if (c == '\n') {
    char cr = '\r';
    mp_hal_stdout_tx_strn(&cr, 1);
  }

  mp_hal_stdout_tx_strn(&c, 1);
}

static inline int modzm_printf(const char *format, ...) {
  va_list args;
  va_start(args, format);

  vstr_t vstr;
  vstr_init(&vstr, 64);
  vstr_vprintf(&vstr, format, args);
  va_end(args);

  if (vstr.len > 0) {
    mp_hal_stdout_tx_strn(vstr.buf, vstr.len);
  }

  int len = vstr.len;
  vstr_clear(&vstr);
  return len;
}

static inline FILE *modzm_fopen(const char *path, const char *mode) {
  // Convert C strings to MicroPython Objects
  mp_obj_t path_obj = mp_obj_new_str(path, strlen(path));
  mp_obj_t mode_obj = mp_obj_new_str(mode, strlen(mode));

  // Look up the standard 'open' function that Python uses
  // This function knows all about the /flash mount point
  mp_obj_t open_fn = mp_load_global(MP_QSTR_open);

  // Call it: open(path, mode)
  nlr_buf_t nlr;
  if (nlr_push(&nlr) == 0) {
    mp_obj_t file_obj =
        mp_call_function_n_kw(open_fn, 2, 0, (mp_obj_t[]){path_obj, mode_obj});
    nlr_pop();
    return (FILE *)file_obj; // Success: Return the object pointer as our "fd"
  } else {
    // If Python threw an exception (like ENOENT), we catch it here
    return NULL;
  }
}

// --- VFS-Aware Read ---
static inline size_t modzm_fread(void *ptr, size_t size, size_t nmemb,
                              FILE *stream) {
  mp_obj_t stream_obj = (mp_obj_t)stream;
  int errcode = 0;

  // mp_stream_rw reads directly from the Python file object into your C buffer
  mp_uint_t bytes_read =
      mp_stream_rw(stream_obj, ptr, size * nmemb, &errcode, MP_STREAM_RW_READ);

  if (errcode != 0 || bytes_read == MP_STREAM_ERROR) {
    return 0; // Read failed
  }

  // fread expects the return value to be the number of ITEMS read, not bytes
  return bytes_read / size;
}

// --- VFS-Aware Seek ---
static inline int modzm_fseek(FILE *stream, long offset, int whence) {
  mp_obj_t stream_obj = (mp_obj_t)stream;
  int errcode = 0;

  // whence maps perfectly to MicroPython's seek (0=SET, 1=CUR, 2=END)
  mp_uint_t res = mp_stream_seek(stream_obj, offset, whence, &errcode);

  if (errcode != 0 || res == MP_STREAM_ERROR) {
    return -1; // Seek failed
  }
  return 0; // Success
}

// --- VFS-Aware Tell (often needed by fseek logic) ---
static inline long modzm_ftell(FILE *stream) {
  mp_obj_t stream_obj = (mp_obj_t)stream;
  int errcode = 0;

  // Seeking 0 bytes from the CURRENT position returns the absolute offset
  mp_uint_t res = mp_stream_seek(stream_obj, 0, SEEK_CUR, &errcode);

  if (errcode != 0 || res == MP_STREAM_ERROR) {
    return -1L;
  }
  return (long)res;
}

// --- VFS-Aware Close ---
static inline int modzm_fclose(FILE *stream) {
  mp_obj_t stream_obj = (mp_obj_t)stream;
  mp_stream_close(stream_obj);
  return 0; // Success
}

#endif
