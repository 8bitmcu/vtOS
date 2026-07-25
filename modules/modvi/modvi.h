/*
 * MicroPython modvi Interface Library
 * Copyright (c) 2026 8bitmcu
 * License: MIT
 */

#ifndef MODVI_H
#define MODVI_H

#include <stdint.h>
#include "py/obj.h"
#include "py/stream.h"

typedef struct _modvi_vi_obj_t {
  mp_obj_base_t base;
  mp_obj_t stream_obj;
  const mp_stream_p_t *stream_p;
  uint16_t width;
  uint16_t height;
} modvi_vi_obj_t;

// Set for the duration of one edit session -- modvi_term.c's term_read()
// reaches through this to get at the KVM stream object, same pattern as
// modules/vi/vi.c's current_vi_instance.
extern modvi_vi_obj_t *current_modvi_instance;

#endif
