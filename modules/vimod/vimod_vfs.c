/*
 * vimod_vfs.c -- MicroPython VFS bridge for neatvi's file I/O.
 * Copyright (c) 2026 8bitmcu
 * License: MIT
 *
 * Ported from modules/vi/vi.c's vfs_open/vfs_read/vfs_write (same
 * approach: route through MicroPython's real open() builtin and stream
 * protocol rather than a real POSIX filesystem, since this device's
 * storage is a MicroPython-VFS-level mount only -- see modules/modssh/
 * myFilesystem.h for the original precedent this and modules/vi/ both
 * follow). Simplified relative to vi.c's version: neatvi only ever uses
 * plain int fds, never stdio FILE*.
 */

#include "vimod_vfs.h"

#include "py/obj.h"
#include "py/runtime.h"
#include "py/stream.h"

int vfs_open(const char *path, const char *mode) {
  mp_obj_t path_obj = mp_obj_new_str(path, strlen(path));
  mp_obj_t mode_obj = mp_obj_new_str(mode, strlen(mode));
  mp_obj_t open_fn = mp_load_global(MP_QSTR_open);

  nlr_buf_t nlr;
  if (nlr_push(&nlr) == 0) {
    mp_obj_t file_obj =
        mp_call_function_n_kw(open_fn, 2, 0, (mp_obj_t[]){path_obj, mode_obj});
    nlr_pop();
    return (int)file_obj;
  }
  return -1;
}

int vfs_read(int fd, void *buf, size_t n) {
  int errcode;
  const mp_stream_p_t *stream_p =
      mp_get_stream_raise((mp_obj_t)fd, MP_STREAM_OP_READ);
  mp_uint_t n_read = stream_p->read((mp_obj_t)fd, buf, n, &errcode);
  if (n_read == (mp_uint_t)-1) {
    return -1;
  }
  return (int)n_read;
}

int vfs_write(int fd, const void *buf, size_t n) {
  int errcode;
  const mp_stream_p_t *stream_p =
      mp_get_stream_raise((mp_obj_t)fd, MP_STREAM_OP_WRITE);
  mp_uint_t n_written = stream_p->write((mp_obj_t)fd, buf, n, &errcode);
  if (n_written == (mp_uint_t)-1) {
    return -1;
  }
  return (int)n_written;
}

int vfs_close(int fd) {
  mp_obj_t file_obj = (mp_obj_t)fd;
  nlr_buf_t nlr;
  if (nlr_push(&nlr) == 0) {
    mp_obj_t dest[2];
    mp_load_method(file_obj, MP_QSTR_close, dest);
    mp_call_method_n_kw(0, 0, dest);
    nlr_pop();
    return 0;
  }
  return -1;
}

int vfs_readable(const char *path) {
  int fd = vfs_open(path, "rb");
  if (fd < 0) {
    return 0;
  }
  vfs_close(fd);
  return 1;
}
