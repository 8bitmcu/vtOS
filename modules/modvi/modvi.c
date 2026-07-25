/*
 * MicroPython modvi Interface Library
 * Copyright (c) 2026 8bitmcu
 * License: MIT
 *
 * Mirrors modules/vi/vi_module.c's constructor/registration shape exactly
 * -- see that file and this port's plan for why. The actual editor
 * (neatvi, vendored under neatvi/) is a single-instance, fully synchronous
 * program: one call in, one return out, no background task and no
 * concurrent-session support needed.
 */

#include "modvi.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/stream.h"
#include <stdint.h>

modvi_vi_obj_t *current_modvi_instance = NULL;

// Defined in neatvi/vi.c's adapted entry point (see that file's own
// modvi_main() near the bottom, replacing neatvi's original main()).
void modvi_main(char *filename, int width, int height);

// The Constructor: modvi.Vi(env, args)
static mp_obj_t modvi_vi_make_new(const mp_obj_type_t *type, size_t n_args,
                                  size_t n_kw, const mp_obj_t *args) {
  mp_arg_check_num(n_args, n_kw, 2, 2, false);

  modvi_vi_obj_t *self = m_new_obj(modvi_vi_obj_t);
  self->base.type = type;

  mp_obj_t env_obj = args[0];
  mp_obj_t dest_stream[2];

  mp_load_method_maybe(env_obj, qstr_from_str("kvm"), dest_stream);
  if (dest_stream[0] != MP_OBJ_NULL) {
    self->stream_obj = dest_stream[0];
  }
  self->stream_p = mp_get_stream_raise(self->stream_obj,
                                       MP_STREAM_OP_READ | MP_STREAM_OP_WRITE);

  int tw = 40, th = 16;
  mp_obj_t dest[2];

  mp_load_method_maybe(env_obj, MP_QSTR_cols, dest);
  if (dest[0] != MP_OBJ_NULL) {
    tw = mp_obj_get_int(dest[0]);
  }
  mp_load_method_maybe(env_obj, MP_QSTR_rows, dest);
  if (dest[0] != MP_OBJ_NULL) {
    th = mp_obj_get_int(dest[0]);
  }

  self->width = tw;
  self->height = th;

  size_t shell_argc = 0;
  mp_obj_t *shell_argv;
  mp_obj_get_array(args[1], &shell_argc, &shell_argv);

  const char *fname = "";
  if (shell_argc > 0) {
    if (!mp_obj_is_str(shell_argv[0])) {
      mp_raise_TypeError(MP_ERROR_TEXT("Filename must be a string"));
    }
    fname = mp_obj_str_get_str(shell_argv[0]);
  }

  current_modvi_instance = self;

  nlr_buf_t nlr;
  if (nlr_push(&nlr) == 0) {
    modvi_main((char *)fname, self->width, self->height);
    current_modvi_instance = NULL;
    nlr_pop();
  } else {
    // A MicroPython exception (e.g. MemoryError) unwound out of neatvi's
    // own call stack -- see the port's plan for the allocator tradeoff
    // this implies (neatvi's own malloc()-backed buffers up to this
    // point are not reclaimed, unlike toybox vi's GC-tracked heap).
    current_modvi_instance = NULL;
    printf("\n[modvi] Emergency exit: recovered from an exception.\n");
  }

  return MP_OBJ_FROM_PTR(self);
}

static const mp_rom_map_elem_t modvi_vi_locals_dict_table[] = {};
static MP_DEFINE_CONST_DICT(modvi_vi_locals_dict, modvi_vi_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(modvi_type_Vi, MP_QSTR_Vi, MP_TYPE_FLAG_NONE, make_new,
                         modvi_vi_make_new, locals_dict, &modvi_vi_locals_dict);

static mp_obj_t _modvi_main(mp_obj_t env, mp_obj_t args) {
  mp_obj_t ctor_args[2] = {env, args};
  modvi_vi_make_new(&modvi_type_Vi, 2, 0, ctor_args);
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(modvi_main_obj, _modvi_main);

static const mp_rom_map_elem_t modvi_module_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_modvi)},
    {MP_ROM_QSTR(MP_QSTR_Vi), MP_ROM_PTR(&modvi_type_Vi)},
    {MP_ROM_QSTR(MP_QSTR_main), MP_ROM_PTR(&modvi_main_obj)},
};
static MP_DEFINE_CONST_DICT(modvi_module_globals, modvi_module_globals_table);

const mp_obj_module_t modvi_user_module = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&modvi_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_modvi, modvi_user_module);
