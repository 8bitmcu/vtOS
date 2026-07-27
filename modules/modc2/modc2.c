/*
 * modc2.c
 *
 * MicroPython binding for Codec2, David Rowe's low-bitrate speech codec
 * (vendored under vendor/, LGPL-2.1 -- see COPYING in this directory,
 * mirrored from https://github.com/drowe67/codec2).
 *
 * Only the core encode/decode API is exposed here (codec2_create/
 * encode/decode/destroy plus the frame-size queries). The vendored
 * source also carries codec2's FreeDV modem/FEC/OFDM layer -- bundled
 * into upstream's own build as a single library, per its own
 * src/CMakeLists.txt -- but that layer isn't used or exposed here: the
 * actual RF transport in this project is LoRa's own SX1262 radio (see
 * modules/tdeck_lora), not an audio-frequency modem, so only the raw
 * "PCM in, compressed frame out (and back)" codec API is needed.
 *
 * codec2_encode()/codec2_decode() work one fixed-size frame at a time
 * (samples_per_frame() 16-bit PCM samples in, bytes_per_frame() bytes
 * out, or the reverse) -- chunking a live audio stream into frames is
 * left to the Python caller (e.g. against AudioRecorder/AudioPlayer's
 * own buffers), same division of responsibility as the rest of this
 * project's audio modules.
 */

#include "py/mperrno.h"
#include "py/runtime.h"

#include "codec2.h"

typedef struct _codec2_obj_t {
  mp_obj_base_t base;
  struct CODEC2 *state;
  int samples_per_frame;
  int bytes_per_frame;
} codec2_obj_t;

extern const mp_obj_type_t codec2_Codec2_type;

// Codec2(mode) -- mode is one of the modc2.MODE_* constants.
static mp_obj_t codec2obj_make_new(const mp_obj_type_t *type, size_t n_args,
                                   size_t n_kw, const mp_obj_t *args) {
  enum { ARG_mode };
  static const mp_arg_t allowed_args[] = {
      {MP_QSTR_mode, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
  };
  mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)];
  mp_arg_parse_all_kw_array(n_args, n_kw, args, MP_ARRAY_SIZE(allowed_args),
                            allowed_args, parsed);

  codec2_obj_t *self = mp_obj_malloc(codec2_obj_t, type);
  self->state = codec2_create(parsed[ARG_mode].u_int);
  if (self->state == NULL) {
    mp_raise_ValueError(
        MP_ERROR_TEXT("invalid Codec2 mode, or out of memory"));
  }

  self->samples_per_frame = codec2_samples_per_frame(self->state);
  self->bytes_per_frame = codec2_bytes_per_frame(self->state);

  return MP_OBJ_FROM_PTR(self);
}

// encode(pcm_bytes) -> bytes
// pcm_bytes must be exactly samples_per_frame() * 2 bytes of 16-bit PCM.
// Returns bytes_per_frame() bytes of compressed frame data.
static mp_obj_t codec2obj_encode(mp_obj_t self_in, mp_obj_t pcm_in) {
  codec2_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (self->state == NULL) {
    mp_raise_msg(&mp_type_RuntimeError,
                 MP_ERROR_TEXT("deinit() already called"));
  }

  mp_buffer_info_t pcm;
  mp_get_buffer_raise(pcm_in, &pcm, MP_BUFFER_READ);

  size_t want = (size_t)self->samples_per_frame * sizeof(int16_t);
  if (pcm.len != want) {
    mp_raise_ValueError(
        MP_ERROR_TEXT("pcm buffer must be exactly samples_per_frame()*2 bytes"));
  }

  vstr_t vstr;
  vstr_init_len(&vstr, self->bytes_per_frame);
  codec2_encode(self->state, (uint8_t *)vstr.buf, (short *)pcm.buf);

  return mp_obj_new_bytes_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_2(codec2obj_encode_obj, codec2obj_encode);

// decode(frame_bytes) -> bytes
// frame_bytes must be exactly bytes_per_frame() bytes of compressed data.
// Returns samples_per_frame() * 2 bytes of 16-bit PCM.
static mp_obj_t codec2obj_decode(mp_obj_t self_in, mp_obj_t frame_in) {
  codec2_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (self->state == NULL) {
    mp_raise_msg(&mp_type_RuntimeError,
                 MP_ERROR_TEXT("deinit() already called"));
  }

  mp_buffer_info_t frame;
  mp_get_buffer_raise(frame_in, &frame, MP_BUFFER_READ);

  if (frame.len != (size_t)self->bytes_per_frame) {
    mp_raise_ValueError(
        MP_ERROR_TEXT("frame buffer must be exactly bytes_per_frame() bytes"));
  }

  vstr_t vstr;
  vstr_init_len(&vstr, self->samples_per_frame * sizeof(int16_t));
  codec2_decode(self->state, (short *)vstr.buf, (const uint8_t *)frame.buf);

  return mp_obj_new_bytes_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_2(codec2obj_decode_obj, codec2obj_decode);

static mp_obj_t codec2obj_samples_per_frame(mp_obj_t self_in) {
  codec2_obj_t *self = MP_OBJ_TO_PTR(self_in);
  return mp_obj_new_int(self->samples_per_frame);
}
static MP_DEFINE_CONST_FUN_OBJ_1(codec2obj_samples_per_frame_obj,
                                 codec2obj_samples_per_frame);

static mp_obj_t codec2obj_bytes_per_frame(mp_obj_t self_in) {
  codec2_obj_t *self = MP_OBJ_TO_PTR(self_in);
  return mp_obj_new_int(self->bytes_per_frame);
}
static MP_DEFINE_CONST_FUN_OBJ_1(codec2obj_bytes_per_frame_obj,
                                 codec2obj_bytes_per_frame);

static mp_obj_t codec2obj_deinit(mp_obj_t self_in) {
  codec2_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (self->state != NULL) {
    codec2_destroy(self->state);
    self->state = NULL;
  }
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(codec2obj_deinit_obj, codec2obj_deinit);

static const mp_rom_map_elem_t codec2obj_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_encode), MP_ROM_PTR(&codec2obj_encode_obj)},
    {MP_ROM_QSTR(MP_QSTR_decode), MP_ROM_PTR(&codec2obj_decode_obj)},
    {MP_ROM_QSTR(MP_QSTR_samples_per_frame),
     MP_ROM_PTR(&codec2obj_samples_per_frame_obj)},
    {MP_ROM_QSTR(MP_QSTR_bytes_per_frame),
     MP_ROM_PTR(&codec2obj_bytes_per_frame_obj)},
    {MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&codec2obj_deinit_obj)},
};
static MP_DEFINE_CONST_DICT(codec2obj_locals_dict, codec2obj_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(codec2_Codec2_type, MP_QSTR_Codec2, MP_TYPE_FLAG_NONE,
                         make_new, codec2obj_make_new, locals_dict,
                         &codec2obj_locals_dict);

static const mp_rom_map_elem_t modc2_module_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_modc2)},
    {MP_ROM_QSTR(MP_QSTR_Codec2), MP_ROM_PTR(&codec2_Codec2_type)},
    {MP_ROM_QSTR(MP_QSTR_MODE_3200), MP_ROM_INT(CODEC2_MODE_3200)},
    {MP_ROM_QSTR(MP_QSTR_MODE_2400), MP_ROM_INT(CODEC2_MODE_2400)},
    {MP_ROM_QSTR(MP_QSTR_MODE_1600), MP_ROM_INT(CODEC2_MODE_1600)},
    {MP_ROM_QSTR(MP_QSTR_MODE_1400), MP_ROM_INT(CODEC2_MODE_1400)},
    {MP_ROM_QSTR(MP_QSTR_MODE_1300), MP_ROM_INT(CODEC2_MODE_1300)},
    {MP_ROM_QSTR(MP_QSTR_MODE_1200), MP_ROM_INT(CODEC2_MODE_1200)},
    {MP_ROM_QSTR(MP_QSTR_MODE_700C), MP_ROM_INT(CODEC2_MODE_700C)},
    // NOTE: no MODE_450/MODE_450PWB -- those exist in some codec2 forks
    // but not in this vendored copy (upstream drowe67/codec2, main
    // branch); modes.h only defines 3200/2400/1600/1400/1300/1200/700C.
};
static MP_DEFINE_CONST_DICT(modc2_module_globals, modc2_module_globals_table);

const mp_obj_module_t modc2_user_cmodule = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&modc2_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_modc2, modc2_user_cmodule);
