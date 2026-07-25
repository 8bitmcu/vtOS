/*
 * modvi_vfs.h -- MicroPython VFS bridge for neatvi's file I/O.
 * Copyright (c) 2026 8bitmcu
 * License: MIT
 *
 * neatvi only ever uses plain POSIX int fds (open/read/write/close), never
 * stdio FILE* -- confirmed by grepping the vendored source -- so this is
 * simpler than modules/vi/vi.c's bridge, which needs both.
 */

#ifndef MODVI_VFS_H
#define MODVI_VFS_H

#include <stddef.h>

// mode is a Python-style string ("rb"/"wb"), not a POSIX O_* bitmask --
// returns a cast MicroPython file-object pointer as the "fd", or -1 on
// failure (matches vi.c's vfs_open() convention exactly).
int vfs_open(const char *path, const char *mode);
int vfs_read(int fd, void *buf, size_t n);
int vfs_write(int fd, const void *buf, size_t n);
int vfs_close(int fd);

// Existence/readability probe used in place of access(path, R_OK) and
// stat()-based mtime() -- see this port's plan for why: real stat()/
// mtime()-based conflict detection isn't meaningful on a single-user
// embedded device, so this substitutes a plain open-for-read probe
// rather than depending on an unverified os.stat() C API path. Returns
// 1 if the path can be opened for reading, 0 otherwise.
int vfs_readable(const char *path);

#endif
