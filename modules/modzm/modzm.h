/*
 * MicroPython Frotz Interface Library
 * Copyright (c) 2026 8bitmcu
 * License: MIT
 */

#ifndef MODZM_H
#define MODZM_H

#include "py/obj.h"
#include "py/stream.h"
#include <stdint.h>

typedef struct _modzm_zm_obj_t {
  mp_obj_base_t base;
  mp_obj_t env_obj;
  mp_obj_t stream_obj;
  const mp_stream_p_t *stream_p;
  mp_obj_t args_obj;
} modzm_zm_obj_t;

#endif
