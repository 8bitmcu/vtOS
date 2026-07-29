#include "EspHal.h"
#include <RadioLib.h>

// Wrap all MicroPython includes in extern "C" so the C++ compiler doesn't
// mangle them
extern "C" {
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/objstr.h"
#include "py/runtime.h"
}

// -------------------------------------------------------------------------
// 1. Object Structure
// -------------------------------------------------------------------------
typedef struct _lora_obj_t {
  mp_obj_base_t base;
  EspHal *hal;
  Module *mod;
  SX1262 *radio;
} lora_obj_t;

extern "C" const mp_obj_type_t lora_LoRa_type;

// -------------------------------------------------------------------------
// 2. Python-Visible Methods
// -------------------------------------------------------------------------

// LoRa(cs, dio1, rst, busy, sck, miso, mosi) -- all may be given positionally
// or by keyword.
static mp_obj_t lora_make_new(const mp_obj_type_t *type, size_t n_args,
                              size_t n_kw, const mp_obj_t *args) {
  enum { ARG_cs, ARG_dio1, ARG_rst, ARG_busy, ARG_sck, ARG_miso, ARG_mosi };
  static const mp_arg_t allowed_args[] = {
      {MP_QSTR_cs, MP_ARG_INT | MP_ARG_REQUIRED, {.u_int = 0}},
      {MP_QSTR_dio1, MP_ARG_INT | MP_ARG_REQUIRED, {.u_int = 0}},
      {MP_QSTR_rst, MP_ARG_INT | MP_ARG_REQUIRED, {.u_int = 0}},
      {MP_QSTR_busy, MP_ARG_INT | MP_ARG_REQUIRED, {.u_int = 0}},
      {MP_QSTR_sck, MP_ARG_INT | MP_ARG_REQUIRED, {.u_int = 0}},
      {MP_QSTR_miso, MP_ARG_INT | MP_ARG_REQUIRED, {.u_int = 0}},
      {MP_QSTR_mosi, MP_ARG_INT | MP_ARG_REQUIRED, {.u_int = 0}},
  };
  mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)];
  mp_map_t kw_args;
  mp_map_init_fixed_table(&kw_args, n_kw, args + n_args);
  mp_arg_parse_all(n_args, args, &kw_args, MP_ARRAY_SIZE(allowed_args),
                   allowed_args, parsed);

  lora_obj_t *self = m_new_obj(lora_obj_t);
  self->base.type = type;

  int cs = parsed[ARG_cs].u_int;
  int dio1 = parsed[ARG_dio1].u_int;
  int rst = parsed[ARG_rst].u_int;
  int busy = parsed[ARG_busy].u_int;
  int sck = parsed[ARG_sck].u_int;
  int miso = parsed[ARG_miso].u_int;
  int mosi = parsed[ARG_mosi].u_int;

  // Initialize RadioLib's ESP32 HAL with the T-Deck's custom SPI pins (Using
  // SPI2 / FSPI)
  self->hal = new EspHal(sck, miso, mosi, SPI2_HOST);
  self->mod = new Module(self->hal, cs, dio1, rst, busy);
  self->radio = new SX1262(self->mod);

  return MP_OBJ_FROM_PTR(self);
}

// begin(freq, bw, sf, cr, sync_word=0x12, power=10) -- freq/bw/sf/cr may be
// given positionally or by keyword; sync_word/power are optional keywords.
static mp_obj_t lora_begin(size_t n_args, const mp_obj_t *pos_args,
                           mp_map_t *kw_args) {
  lora_obj_t *self = (lora_obj_t *)MP_OBJ_TO_PTR(pos_args[0]);

  enum { ARG_freq, ARG_bw, ARG_sf, ARG_cr, ARG_sync_word, ARG_power };
  static const mp_arg_t allowed_args[] = {
      {MP_QSTR_freq, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL}},
      {MP_QSTR_bw, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL}},
      {MP_QSTR_sf, MP_ARG_INT | MP_ARG_REQUIRED, {.u_int = 0}},
      {MP_QSTR_cr, MP_ARG_INT | MP_ARG_REQUIRED, {.u_int = 0}},
      {MP_QSTR_sync_word, MP_ARG_INT, {.u_int = 0x12}},
      {MP_QSTR_power, MP_ARG_INT, {.u_int = 10}},
  };
  mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
  mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                   MP_ARRAY_SIZE(allowed_args), allowed_args, args);

  float freq = mp_obj_get_float(args[ARG_freq].u_obj);
  float bw = mp_obj_get_float(args[ARG_bw].u_obj);
  int sf = args[ARG_sf].u_int;
  int cr = args[ARG_cr].u_int;
  int sync = args[ARG_sync_word].u_int;
  int power = args[ARG_power].u_int;

  int state = self->radio->begin(freq, bw, sf, cr, sync, power);
  if (state != RADIOLIB_ERR_NONE) {
    mp_raise_msg_varg(&mp_type_RuntimeError,
                      MP_ERROR_TEXT("RadioLib begin failed with code %d"),
                      state);
  }

  return mp_obj_new_int(state);
}

static MP_DEFINE_CONST_FUN_OBJ_KW(lora_begin_obj, 1, lora_begin);

// transmit(bytes)
static mp_obj_t lora_transmit(mp_obj_t self_in, mp_obj_t data_in) {
  lora_obj_t *self = (lora_obj_t *)MP_OBJ_TO_PTR(self_in);

  mp_buffer_info_t bufinfo;
  mp_get_buffer_raise(data_in, &bufinfo, MP_BUFFER_READ);

  int state = self->radio->transmit((uint8_t *)bufinfo.buf, bufinfo.len);
  if (state != RADIOLIB_ERR_NONE) {
    mp_raise_msg_varg(&mp_type_RuntimeError,
                      MP_ERROR_TEXT("Transmit failed: %d"), state);
  }
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(lora_transmit_obj, lora_transmit);

// start_receive() -> puts the chip into RX mode.
// Must be called before receive() will ever see a packet: begin() leaves the
// chip in standby, and it does not start listening on its own. After a
// packet is read, the chip drops back to standby, so this must be called
// again to resume listening (see RadioLib's own examples: "put module back
// into listen mode").
static mp_obj_t lora_start_receive(mp_obj_t self_in) {
  lora_obj_t *self = (lora_obj_t *)MP_OBJ_TO_PTR(self_in);

  int state = self->radio->startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    mp_raise_msg_varg(&mp_type_RuntimeError,
                      MP_ERROR_TEXT("startReceive failed with code %d"),
                      state);
  }

  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lora_start_receive_obj, lora_start_receive);

// receive() -> returns bytes or None
static mp_obj_t lora_receive(mp_obj_t self_in) {
  lora_obj_t *self = (lora_obj_t *)MP_OBJ_TO_PTR(self_in);

  // getPacketLength() reflects the *last* received packet and stays
  // non-zero even after it's already been read -- it does not mean a new
  // packet has arrived. Only the RX_DONE IRQ (mirrored on the DIO1 pin)
  // tells us that. Without this check, the same packet gets re-read and
  // re-reported on every poll.
  if (!self->hal->digitalRead(self->mod->getIrq())) {
    return mp_const_none; // No new packet since the last read
  }

  int len = self->radio->getPacketLength();
  if (len <= 0) {
    return mp_const_none; // Nothing to read
  }

  vstr_t vstr;
  vstr_init_len(&vstr, len);
  int state = self->radio->readData((uint8_t *)vstr.buf, len);

  if (state == RADIOLIB_ERR_NONE) {
    // Correct usage: pass ONLY the vstr pointer and use bytes constructor
    mp_obj_t ret = mp_obj_new_bytes_from_vstr(&vstr);
    vstr_clear(
        &vstr); // Clean up the vstr buffer after creating the bytes object
    return ret;
  }

  vstr_clear(&vstr);
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lora_receive_obj, lora_receive);

// rssi() -> float
static mp_obj_t lora_rssi(mp_obj_t self_in) {
  lora_obj_t *self = (lora_obj_t *)MP_OBJ_TO_PTR(self_in);
  return mp_obj_new_float(self->radio->getRSSI());
}
static MP_DEFINE_CONST_FUN_OBJ_1(lora_rssi_obj, lora_rssi);

// 1. Create a quick subclass to bypass the 'protected' restriction
class SX1262_Exposed : public SX1262 {
public:
  // This public method can legally call the protected method inside the parent
  // class
  uint8_t publicGetStatus() { return this->getStatus(); }
  uint16_t publicGetDeviceErrors() { return this->getDeviceErrors(); }
};

// 2. get_status() -> int
static mp_obj_t lora_get_status(mp_obj_t self_in) {
  lora_obj_t *self = (lora_obj_t *)MP_OBJ_TO_PTR(self_in);

  // Cast our radio pointer to the exposed class and read the status
  uint8_t status = ((SX1262_Exposed *)self->radio)->publicGetStatus();

  return mp_obj_new_int(status);
}
static MP_DEFINE_CONST_FUN_OBJ_1(lora_get_status_obj, lora_get_status);

// get_device_errors() -> int
// NOTE: unlike get_status(), RadioLib's getDeviceErrors() does a real 2-byte
// SPI readback (SX126x_commands.cpp), so this reflects genuine chip state.
static mp_obj_t lora_get_device_errors(mp_obj_t self_in) {
  lora_obj_t *self = (lora_obj_t *)MP_OBJ_TO_PTR(self_in);

  uint16_t errors = ((SX1262_Exposed *)self->radio)->publicGetDeviceErrors();

  return mp_obj_new_int(errors);
}
static MP_DEFINE_CONST_FUN_OBJ_1(lora_get_device_errors_obj, lora_get_device_errors);

// deinit() -- sleeps the radio and releases the SPI device handle + the
// heap-allocated RadioLib/HAL objects (EspHal::~EspHal() -> term() ->
// spiEnd() removes just our device from the shared SPI bus, not the
// whole bus -- see EspHal::spiBegin()'s shared-mode path). Safe to call
// more than once; a second call is a no-op. Any other method called
// after this will crash (self->radio etc. are null by then) -- the
// Python side is expected to drop its reference (env.radio = None)
// immediately after calling this, matching env.ble/env.audio's
// teardown-on-exit pattern.
static mp_obj_t lora_deinit(mp_obj_t self_in) {
  lora_obj_t *self = (lora_obj_t *)MP_OBJ_TO_PTR(self_in);

  if (self->radio == nullptr) {
    return mp_const_none; // already deinitialized
  }

  self->radio->sleep();

  delete self->radio;
  delete self->mod;
  delete self->hal;

  self->radio = nullptr;
  self->mod = nullptr;
  self->hal = nullptr;

  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lora_deinit_obj, lora_deinit);

// -------------------------------------------------------------------------
// 3. Module Registration (Must be in extern "C")
// -------------------------------------------------------------------------
extern "C" {

static const mp_rom_map_elem_t lora_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_begin), MP_ROM_PTR(&lora_begin_obj)},
    {MP_ROM_QSTR(MP_QSTR_transmit), MP_ROM_PTR(&lora_transmit_obj)},
    {MP_ROM_QSTR(MP_QSTR_start_receive), MP_ROM_PTR(&lora_start_receive_obj)},
    {MP_ROM_QSTR(MP_QSTR_receive), MP_ROM_PTR(&lora_receive_obj)},
    {MP_ROM_QSTR(MP_QSTR_rssi), MP_ROM_PTR(&lora_rssi_obj)},
    {MP_ROM_QSTR(MP_QSTR_get_status), MP_ROM_PTR(&lora_get_status_obj)},
    {MP_ROM_QSTR(MP_QSTR_get_device_errors), MP_ROM_PTR(&lora_get_device_errors_obj)},
    {MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&lora_deinit_obj)},
};
static MP_DEFINE_CONST_DICT(lora_locals_dict, lora_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(lora_LoRa_type, MP_QSTR_LoRa, MP_TYPE_FLAG_NONE,
                         make_new, (const void *)lora_make_new, locals_dict,
                         (const void *)&lora_locals_dict);

static const mp_rom_map_elem_t lora_module_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_lora)},
    {MP_ROM_QSTR(MP_QSTR_LoRa), MP_ROM_PTR(&lora_LoRa_type)},
};
static MP_DEFINE_CONST_DICT(lora_module_globals, lora_module_globals_table);

extern const mp_obj_module_t lora_user_cmodule = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&lora_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_lora, lora_user_cmodule);

} // end extern "C"
