/*
 * MicroPython vimod Interface Library
 * Copyright (c) 2026 8bitmcu
 * License: MIT
 */

#ifndef VIMOD_MODULE_H
#define VIMOD_MODULE_H

#include <stdint.h>
#include "py/obj.h"
#include "py/stream.h"

typedef struct _vimod_vi_obj_t {
  mp_obj_base_t base;
  mp_obj_t stream_obj;
  const mp_stream_p_t *stream_p;
  uint16_t width;
  uint16_t height;
} vimod_vi_obj_t;

// Set for the duration of one edit session -- vimod_term.c's term_read()
// reaches through this to get at the KVM stream object, same pattern as
// modules/vi/vi.c's current_vi_instance.
extern vimod_vi_obj_t *current_vimod_instance;

#endif
