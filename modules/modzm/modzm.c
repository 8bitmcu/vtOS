/*
 * MicroPython Frotz Interface Library
 * Copyright (c) 2026 8bitmcu
 * License: MIT
 */

#include "modzm.h"
#include "py/mpconfig.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "py/runtime.h"
#include <setjmp.h>
#include <stdio.h>

extern int frotz_main(int argc, char *argv[]);

jmp_buf frotz_exit_env;

// Global pointer for frotz_main to access the current instance's stream
modzm_zm_obj_t *current_frotz_instance = NULL;

// New constructor signature: modzm.ZMachine(env, args)
static mp_obj_t modzm_make_new(const mp_obj_type_t *type, size_t n_args,
                            size_t n_kw, const mp_obj_t *args) {
  // Expects exactly 2 arguments: the stream (env) and the tuple/list of shell
  // args
  mp_arg_check_num(n_args, n_kw, 2, 2, false);

  modzm_zm_obj_t *self = m_new_obj(modzm_zm_obj_t);
  self->base.type = type;

  // Handle Stream / Environment (args[0])
  mp_obj_t env_obj = args[0];
  self->env_obj = env_obj;
  mp_obj_t dest_stream[2];

  // Extract the actual underlying stream object ('kvm') from env
  mp_load_method_maybe(env_obj, qstr_from_str("kvm"), dest_stream);
  if (dest_stream[0] != MP_OBJ_NULL) {
    self->stream_obj = dest_stream[0];
  }
  // Safely query the stream protocol on the extracted stream object
  self->stream_p = mp_get_stream_raise(self->stream_obj,
                                       MP_STREAM_OP_READ | MP_STREAM_OP_WRITE);

  // Store the raw MicroPython tuple/list containing shell arguments
  self->args_obj = args[1];

  current_frotz_instance = self;

  return MP_OBJ_FROM_PTR(self);
}

// A method: z.run()
static mp_obj_t modzm_run(mp_obj_t self_in) {
  modzm_zm_obj_t *self = MP_OBJ_TO_PTR(self_in);

  // Dynamically read .cols and .rows from Python 'env' object
  // (qstr_from_str ensures it resolves correctly at runtime)
  int tw = mp_obj_get_int(mp_load_attr(self->env_obj, qstr_from_str("cols")));
  int th = mp_obj_get_int(mp_load_attr(self->env_obj, qstr_from_str("rows")));

  char width_str[16];
  char height_str[16];
  snprintf(width_str, sizeof(width_str), "%d", tw);
  snprintf(height_str, sizeof(height_str), "%d", th);

  // Extract the user-typed arguments from shell tuple/list
  size_t shell_argc = 0;
  mp_obj_t *shell_argv;
  mp_obj_get_array(self->args_obj, &shell_argc, &shell_argv);

  // 3. Calculate total argc: 5 hardcoded slots + whatever the user typed
  // Slots: [0]: "dfrotz", [1]: "-w", [2]: width, [3]: "-h", [4]: height
  int total_argc = 5 + shell_argc;

  // 4. Allocate the array on the stack (+1 for the trailing NULL)
  char *dummy_argv[total_argc + 1];

  // 5. Inject auto-configuration flags
  dummy_argv[0] = "dfrotz";
  dummy_argv[1] = "-w";
  dummy_argv[2] = width_str;
  dummy_argv[3] = "-h";
  dummy_argv[4] = height_str;

  // 6. Append the user's shell arguments (like the filename or extra flags)
  for (size_t i = 0; i < shell_argc; i++) {
    if (!mp_obj_is_str(shell_argv[i])) {
      mp_raise_TypeError(MP_ERROR_TEXT("All shell arguments must be strings"));
    }
    dummy_argv[5 + i] = (char *)mp_obj_str_get_str(shell_argv[i]);
  }
  dummy_argv[total_argc] = NULL; // Terminate array securely

  // 7. Execute Frotz safely
  if (setjmp(frotz_exit_env) == 0) {
    frotz_main(total_argc, dummy_argv);
  }

  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_1(modzm_run_obj, modzm_run);

// Register methods in the class dictionary
static const mp_rom_map_elem_t modzm_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_run), MP_ROM_PTR(&modzm_run_obj)},
};
static MP_DEFINE_CONST_DICT(modzm_locals_dict, modzm_locals_dict_table);

// Define the class type
MP_DEFINE_CONST_OBJ_TYPE(modzm_type, MP_QSTR_ZMachine, MP_TYPE_FLAG_NONE, make_new,
                         modzm_make_new, locals_dict, &modzm_locals_dict);

static mp_obj_t modzm_main(mp_obj_t env, mp_obj_t args) {
  mp_obj_t ctor_args[2] = {env, args};
  mp_obj_t m = modzm_make_new(&modzm_type, 2, 0, ctor_args);
  return modzm_run(m);
}
static MP_DEFINE_CONST_FUN_OBJ_2(modzm_main_obj, modzm_main);

// Register the module
static const mp_rom_map_elem_t modzm_module_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_modzm)},
    {MP_ROM_QSTR(MP_QSTR_ZMachine), MP_ROM_PTR(&modzm_type)},
    {MP_ROM_QSTR(MP_QSTR_main), MP_ROM_PTR(&modzm_main_obj)},
};
static MP_DEFINE_CONST_DICT(modzm_module_globals, modzm_module_globals_table);

const mp_obj_module_t modzm_user_cmodule = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&modzm_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_modzm, modzm_user_cmodule);
