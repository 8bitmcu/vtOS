/*
 * audiorecorder.c
 *
 * MicroPython C module: microphone recording to a WAV file, for the
 * LilyGO T-Deck's onboard ES7210 mic array (also usable with any other
 * ES7210-based board -- pins are all constructor arguments).
 *
 * This is compiled into the same usermod as audioplayer.c (see
 * micropython.cmake) and registers its type into that module's
 * namespace, so from Python it's `from audioplayer import AudioRecorder`
 * -- but it's a separate translation unit because it's a genuinely
 * separate signal path (I2C-configured ADC + I2S RX) that doesn't share
 * any code with MP3/WAV playback beyond the ring buffer helper (shared
 * via ring_buf.h) and the general VFS/task architecture.
 *
 * The ES7210 itself is configured over I2C using Espressif's own
 * "espressif/es7210" component (add via idf_component.yml, same idea as
 * esp-libhelix-mp3 for the MP3 decoder -- see README.md). That component
 * only handles the I2C control path; I2S RX (the actual audio data path)
 * is set up here directly with the i2s_std driver, same as audioplayer.c
 * uses for TX.
 *
 * Architecture mirrors audioplayer.c, just with the data flowing the
 * other way: a background FreeRTOS task pulls PCM from I2S RX and pushes
 * it into a ring buffer; the MicroPython thread drains that ring buffer
 * out to a VFS file (record()/stop(), and automatically via
 * mp_sched_schedule() the same way audioplayer.c's feed() works in
 * reverse). See audioplayer.c's file-header comment (in this same
 * module) for the background on *why* it's built this way -- VFS-only
 * file access from the MicroPython thread, watchdog-safe yielding in the
 * background task, etc.
 *
 * WAV output: a 44-byte canonical PCM header is written up front with a
 * data size of 0 (so a recording that's never cleanly stopped still
 * leaves behind a valid, if empty-looking, WAV file rather than a
 * headerless blob), then raw 16-bit PCM is appended as it's captured.
 * stop() seeks back and patches the header's size fields with the real
 * total once recording ends -- so *always* call stop() to finalize a
 * recording, even if you passed record(..., seconds=N) and it will
 * auto-stop capturing on its own; auto-stop only ends the background
 * task, it doesn't rewrite the header or close the file for you.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/runtime.h"
#include "py/stream.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
// See audioplayer.c's identical comment: xTaskCreatePinnedToCore moved
// here in ESP-IDF 5.3+.
#if __has_include("freertos/idf_additions.h")
#include "freertos/idf_additions.h"
#endif

#include "driver/i2c.h"     // legacy I2C driver -- see README for why
#include "driver/i2s_std.h" // same standard I2S driver audioplayer.c uses for TX
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "es7210.h" // from the espressif/es7210 ESP-IDF component

#include "ring_buf.h" // shared with audioplayer.c

static const char *TAG = "audiorecorder";

// ESP_LOGE() only ever reaches ESP-IDF's own console (USB-Serial/JTAG or
// UART0, per CONFIG_ESP_CONSOLE_*) -- os.dupterm() in main.py redirects
// MicroPython's own stdout to the on-device LCD/keyboard terminal, but
// that redirection doesn't touch ESP-IDF's logging backend at all, so
// anyone running this untethered (no PC serial monitor attached) never
// sees these init-time errors. Mirroring them through
// mp_hal_stdout_tx_strn() puts them wherever MicroPython's stdout is
// currently pointed instead. These only fire on rare, non-perf-critical
// init failures, so the extra formatting cost doesn't matter.
static void log_err_to_console(const char *fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0) {
    return;
  }
  if ((size_t)n >= sizeof(buf)) {
    n = sizeof(buf) - 1;
  }
  mp_hal_stdout_tx_strn(buf, (size_t)n);
  mp_hal_stdout_tx_strn("\r\n", 2);
}

// ---- tunables -------------------------------------------------------

#define RECORD_RING_BUF_SIZE (24 * 1024) // bytes of captured PCM kept queued
#define RECORD_CHUNK_SIZE                                                      \
  (4 * 1024) // bytes read from I2S RX per iteration -- matches
             // one full DMA descriptor (dma_buf_len=1024 frames
             // * 4 bytes/frame stereo 16-bit) by default; if you
             // change dma_buf_len, this is worth keeping in sync
#define RECORD_DRAIN_CHUNK_SIZE                                                \
  (2 * 1024)                  // bytes written to the VFS file per drain
#define RECORD_DRAIN_ROUNDS 8 // drain calls done per scheduled callback
#define RECORD_TASK_STACK_WORDS 6144
#define RECORD_TASK_PRIORITY 5

#define WAV_HEADER_SIZE 44

typedef struct _audiorecorder_obj_t {
  mp_obj_base_t base;

  i2s_port_t port_num;
  i2s_chan_handle_t rx_handle;
  bool i2s_installed;

  i2c_port_t i2c_port;
  bool i2c_installed;
  es7210_dev_handle_t codec;

  int sample_rate;
  int channels; // 1 or 2, 16-bit PCM either way

  // task control
  TaskHandle_t task;
  StaticTask_t task_buf; // xTaskCreateStaticPinnedToCore()'s control block
  StackType_t *stack;    // PSRAM-backed, see audiorecorder_record()
  SemaphoreHandle_t done_sem;
  volatile bool recording;
  volatile bool stop_request;
  volatile int last_error;
  volatile uint32_t total_bytes; // audio data bytes captured so far
  uint32_t
      max_bytes; // 0 = unlimited; else record()'s seconds= converted to bytes
  volatile int16_t last_peak;        // simple peak-level meter, see level()
  volatile uint32_t i2s_error_count; // i2s_channel_read() errors/timeouts
  volatile uint32_t rb_full_stalls;  // ring-buffer-full backpressure events

  // VFS-backed sink: only ever touched from the MicroPython thread
  // (record() / stop() / deinit() / the scheduled drain callback), never
  // from audio_record_task().
  mp_obj_t file_obj;
  volatile bool drain_pending; // a drain callback is already scheduled

  ring_buf_t rb;
} audiorecorder_obj_t;

// Registered into audioplayer.c's module globals table -- not static.
const mp_obj_type_t audiorecorder_AudioRecorder_type;

// ---- WAV header ---------------------------------------------------------

static void put_le16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void build_wav_header(uint8_t *hdr, int sample_rate, int channels,
                             uint32_t data_bytes) {
  const uint16_t bits_per_sample = 16;
  uint32_t byte_rate = (uint32_t)sample_rate * channels * (bits_per_sample / 8);
  uint16_t block_align = (uint16_t)(channels * (bits_per_sample / 8));

  memcpy(hdr + 0, "RIFF", 4);
  put_le32(hdr + 4, 36 + data_bytes);
  memcpy(hdr + 8, "WAVE", 4);
  memcpy(hdr + 12, "fmt ", 4);
  put_le32(hdr + 16, 16); // fmt chunk size
  put_le16(hdr + 20, 1);  // PCM
  put_le16(hdr + 22, (uint16_t)channels);
  put_le32(hdr + 24, (uint32_t)sample_rate);
  put_le32(hdr + 28, byte_rate);
  put_le16(hdr + 32, block_align);
  put_le16(hdr + 34, bits_per_sample);
  memcpy(hdr + 36, "data", 4);
  put_le32(hdr + 40, data_bytes);
}

// ---- VFS drain (runs on the MicroPython thread only) ---------------------
//
// The reverse of audioplayer.c's feed_internal(): pulls up to
// RECORD_DRAIN_CHUNK_SIZE bytes out of the ring buffer and writes them to
// self->file_obj. Returns: 1 = wrote something, 0 = ring buffer was
// empty, -1 = no file open / write error (see last_error).
static int audiorecorder_drain_internal(audiorecorder_obj_t *self) {
  if (self->file_obj == mp_const_none) {
    return -1;
  }

  size_t avail = rb_available(&self->rb);
  if (avail == 0) {
    return 0;
  }

  uint8_t chunk[RECORD_DRAIN_CHUNK_SIZE];
  size_t to_write = (avail < sizeof(chunk)) ? avail : sizeof(chunk);
  size_t n = rb_read(&self->rb, chunk, to_write);
  if (n == 0) {
    return 0;
  }

  // A stream write() is allowed to write fewer bytes than requested
  // without that being an error (errcode stays 0) -- a real possibility
  // for VFS/SD-card writes (cluster boundaries, write-cache pressure,
  // etc). n bytes are already irreversibly gone from the ring buffer by
  // this point, so a short write that isn't retried here is a silent gap
  // in the recorded audio -- which is a click, not a stall, so it never
  // shows up in rb_full_stalls/i2s_error_count. Loop until the full
  // chunk is flushed or a genuine error occurs.
  const mp_stream_p_t *stream_p =
      mp_get_stream_raise(self->file_obj, MP_STREAM_OP_WRITE);
  size_t total_written = 0;
  while (total_written < n) {
    int errcode = 0;
    mp_uint_t written = stream_p->write(
        self->file_obj, chunk + total_written, n - total_written, &errcode);
    if (errcode != 0 || written == MP_STREAM_ERROR) {
      self->last_error = -errcode;
      return -1;
    }
    if (written == 0) {
      // No error reported, but no progress either -- treat as a failure
      // rather than spin forever.
      self->last_error = -MP_EIO;
      return -1;
    }
    total_written += written;
  }
  return 1;
}

// mp_sched_schedule callback: the capture task asks for this to run on
// the main thread the next time the interpreter is at a safe point, same
// mechanism/reasoning as audioplayer.c's feed scheduling.
static mp_obj_t audiorecorder_drain_scheduled(mp_obj_t self_in) {
  audiorecorder_obj_t *self = MP_OBJ_TO_PTR(self_in);
  self->drain_pending = false;
  for (int i = 0; i < RECORD_DRAIN_ROUNDS; i++) {
    if (audiorecorder_drain_internal(self) <= 0) {
      break;
    }
  }
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiorecorder_drain_scheduled_obj,
                                 audiorecorder_drain_scheduled);

static void audiorecorder_request_drain(audiorecorder_obj_t *self) {
  if (!self->drain_pending) {
    self->drain_pending = true;
    if (!mp_sched_schedule(MP_OBJ_FROM_PTR(&audiorecorder_drain_scheduled_obj),
                           MP_OBJ_FROM_PTR(self))) {
      self->drain_pending = false; // scheduler queue full; we'll ask again
    }
  }
}

// ---- capture task ---------------------------------------------------------

static void audio_record_task(void *arg) {
  audiorecorder_obj_t *self = (audiorecorder_obj_t *)arg;
  uint32_t loop_iters = 0;

  // +4 bytes of headroom for carrying over a partial trailing frame
  // between reads -- see the carry_len handling below.
  uint8_t *chunk = heap_caps_malloc(RECORD_CHUNK_SIZE + 4, MALLOC_CAP_8BIT);
  if (chunk == NULL) {
    ESP_LOGE(TAG, "out of memory allocating capture buffer");
    self->last_error = -MP_ENOMEM;
    goto done;
  }
  size_t carry_len = 0; // 0-3 leftover bytes from a non-frame-aligned read

  while (!self->stop_request) {
    // Same watchdog-safety guarantee as audioplayer.c's decode loop: never
    // go more than a handful of iterations without yielding, regardless
    // of which branch below we're taking.
    if ((++loop_iters & 0x3F) == 0) {
      vTaskDelay(1);
    }

    size_t bytes_read = 0;
    esp_err_t err =
        i2s_channel_read(self->rx_handle, chunk + carry_len, RECORD_CHUNK_SIZE,
                         &bytes_read, pdMS_TO_TICKS(200));
    if (err != ESP_OK || bytes_read == 0) {
      // A timeout here is normal early on (e.g. right after
      // i2s_channel_enable() while the clock settles) but a steady
      // stream of them mid-recording means we're actually losing audio
      // -- self->i2s_error_count is readable from Python via
      // diagnostics(), and also logged (throttled) for anyone who does
      // have idf.py monitor available.
      if (err != ESP_OK) {
        self->i2s_error_count++;
        if ((self->i2s_error_count & 0x1F) == 0) {
          ESP_LOGW(TAG, "i2s_channel_read: %u errors so far (last: %s)",
                   (unsigned)self->i2s_error_count, esp_err_to_name(err));
        }
      }
      continue;
    }

    // i2s_channel_read() is *expected* to only ever hand back whole
    // frames (4 bytes = one 16-bit stereo L+R frame), but that's not
    // something to bet on blindly, especially under the backpressure
    // stalls this task already deals with (see rb_full_stalls below).
    // Getting it wrong even once means every following read starts
    // mid-frame, silently scrambling L and R samples together instead
    // of cleanly separating them -- verified as the actual cause of
    // earlier "click" artifacts by inspecting raw sample values around
    // detected events: chaotic value-to-value jumps between unrelated
    // magnitudes, not the flat/frozen run of repeated samples a true
    // dropout would leave behind. Carry any leftover partial frame over
    // to the front of the buffer instead of assuming chunk[0] always
    // lands on a left-channel sample boundary.
    size_t total = carry_len + bytes_read;
    size_t usable = total - (total % 4);
    carry_len = total - usable;
    if (carry_len > 0) {
      memmove(chunk, chunk + usable, carry_len);
    }
    bytes_read = usable;
    if (bytes_read == 0) {
      continue; // nothing frame-aligned yet; wait for more data
    }

    // I2S RX always receives real stereo (see the comment in make_new());
    // if the caller asked for a mono WAV, downmix here by keeping just
    // the left slot of each interleaved L/R pair, in place. bytes_read
    // is halved accordingly for everything below.
    if (self->channels == 1) {
      int16_t *stereo = (int16_t *)chunk;
      size_t n_frames = bytes_read / (2 * sizeof(int16_t));
      for (size_t i = 0; i < n_frames; i++) {
        stereo[i] = stereo[i * 2];
      }
      bytes_read = n_frames * sizeof(int16_t);
    }

    // Cheap peak-level meter -- handy for a mic-level UI or a rough
    // "is anyone talking" check without pulling in a full VAD library
    // (the Arduino example uses ESP-ADF's esp_vad for that; a proper
    // port of that is a reasonable follow-up if you need real speech
    // detection rather than just a level).
    int16_t peak = 0;
    const int16_t *samples = (const int16_t *)chunk;
    for (size_t i = 0; i < bytes_read / sizeof(int16_t); i++) {
      int16_t mag = samples[i] < 0 ? (int16_t)-samples[i] : samples[i];
      if (mag > peak) {
        peak = mag;
      }
    }
    self->last_peak = peak;

    size_t written = 0;
    while (written < bytes_read && !self->stop_request) {
      size_t n = rb_write(&self->rb, chunk + written, bytes_read - written);
      written += n;
      if (n == 0) {
        // Ring buffer's full -- give the MicroPython thread a moment to
        // drain it (already requested below, every iteration, not just
        // here -- see that comment). Each trip through here is 5ms we're
        // *not* calling i2s_channel_read(): the I2S peripheral keeps
        // capturing into its own hardware DMA ring regardless, and if
        // this stall runs long enough relative to that DMA buffer's
        // size, samples get silently dropped/overwritten at the hardware
        // level -- which sounds exactly like clicks/crackling laid over
        // otherwise-fine audio, not analog noise. Logged (throttled) so
        // it shows up in idf.py monitor rather than only being visible
        // as an unexplained audio artifact.
        self->rb_full_stalls++;
        if ((self->rb_full_stalls & 0x1F) == 0) {
          ESP_LOGW(TAG,
                   "ring buffer full, %u stalls so far -- VFS writes "
                   "may be falling behind capture rate",
                   (unsigned)self->rb_full_stalls);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
      }
    }
    self->total_bytes += (uint32_t)written;

    // Request a drain every iteration, not just when the ring buffer is
    // already full. audiorecorder_request_drain() no-ops if a drain is
    // already pending (see drain_pending), so this is cheap -- but
    // requesting it reactively-only (the previous behavior) meant the
    // VFS/SD-card write never started until backpressure had *already*
    // hit, which is what was producing the rb_full_stalls climbing
    // throughout every recording. Requesting it continuously lets the
    // MicroPython thread keep the ring buffer drained proactively.
    audiorecorder_request_drain(self);

    if (self->max_bytes > 0 && self->total_bytes >= self->max_bytes) {
      break; // hit the optional record(..., seconds=N) limit
    }
  }

  if (self->i2s_error_count > 0 || self->rb_full_stalls > 0) {
    ESP_LOGI(TAG, "capture finished with %u I2S errors, %u ring-buffer stalls",
             (unsigned)self->i2s_error_count, (unsigned)self->rb_full_stalls);
  }

  free(chunk);

done:
  ESP_LOGI(TAG, "capture task exiting (stop_request=%d, total_bytes=%u)",
           self->stop_request, (unsigned)self->total_bytes);
  self->recording = false;
  xSemaphoreGive(self->done_sem);
  vTaskDelete(NULL);
}

// ---- Python-visible methods --------------------------------------------

static mp_obj_t audiorecorder_make_new(const mp_obj_type_t *type, size_t n_args,
                                       size_t n_kw, const mp_obj_t *args) {
  enum {
    ARG_mclk,
    ARG_bck,
    ARG_ws,
    ARG_din,
    ARG_i2c_sda,
    ARG_i2c_scl,
    ARG_i2c_port,
    ARG_i2c_addr,
    ARG_i2s_num,
    ARG_sample_rate,
    ARG_mclk_ratio,
    ARG_channels,
    ARG_mic_gain,
    ARG_mic_bias,
    ARG_dma_buf_count,
    ARG_dma_buf_len,
    ARG_i2c_shared,
  };
  static const mp_arg_t allowed_args[] = {
      {MP_QSTR_mclk, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
      {MP_QSTR_bck, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
      {MP_QSTR_ws, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
      {MP_QSTR_din, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
      {MP_QSTR_i2c_sda, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
      {MP_QSTR_i2c_scl, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
      {MP_QSTR_i2c_port, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0}},
      {MP_QSTR_i2c_addr,
       MP_ARG_KW_ONLY | MP_ARG_INT,
       {.u_int = 0x40}}, // ES7210_ADDRRES_00
      {MP_QSTR_i2s_num, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0}},
      {MP_QSTR_sample_rate, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 16000}},
      // es7210_set_i2s_sample_rate() (espressif/es7210 component) computes
      // mclk_freq_hz = sample_rate_hz * mclk_ratio and looks that exact
      // pair up in a fixed coefficient table -- it's not a free-running
      // PLL, only specific (mclk, rate) combinations are valid. 256 lands
      // on a valid row at the default 16000Hz (256*16000 = 4096000, which
      // is in the table) but NOT at 8000Hz (256*8000 = 2048000, not a
      // valid row) -- 8000Hz needs 512 instead to hit that same 4096000
      // MCLK the table does support at that rate. There's no ratio that
      // works for every sample_rate; pass the right one for whatever rate
      // you actually configure, per the coefficient table in es7210.c.
      {MP_QSTR_mclk_ratio, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 256}},
      {MP_QSTR_channels, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 2}},
      // 0dB, not the more commonly-seen 24dB default (e.g. ESPHome's
      // ES7210 support): LilyGO's own Arduino reference for this exact
      // board calibrates the actually-wired mic pair (MIC1/MIC2) at
      // 0dB, saving the much higher 37.5dB for the other (typically
      // unpopulated) pair. 24dB here was audibly too hot -- amplifying
      // the preamp noise floor right along with the voice signal.
      {MP_QSTR_mic_gain,
       MP_ARG_KW_ONLY | MP_ARG_INT,
       {.u_int = ES7210_MIC_GAIN_0DB}},
      // 2.87V, not the mid-range 2.45V this defaulted to previously:
      // LilyGO's own Arduino reference for this exact board (its vendored
      // lib/es7210, a different driver than this espressif/es7210
      // component but the same chip/hardware) writes 0x70 to both
      // MIC12_BIAS_REG41 and MIC34_BIAS_REG42, which is
      // ES7210_MIC_BIAS_2V87. Under-biasing the mic reduces headroom and
      // raises the noise floor -- this is a closer match to persistent
      // background noise than a gain setting is, since it's independent
      // of the PGA/ADC gain stage entirely.
      {MP_QSTR_mic_bias,
       MP_ARG_KW_ONLY | MP_ARG_INT,
       {.u_int = ES7210_MIC_BIAS_2V87}},
      // Bigger than audioplayer.c's TX defaults on purpose: unlike
      // playback (which can just re-request data), a capture stall here
      // means the hardware DMA ring overflows and silently drops
      // samples -- there's no "try again", it's just gone. 16*1024
      // frames * 4 bytes/frame (stereo 16-bit) = 64KB, ~1s of headroom
      // at 16kHz -- generous margin against SD-card write latency
      // spikes on the VFS drain side, and against the narrower
      // single-sample-dropout signature seen at the previous (32KB)
      // setting, which looked like an occasional descriptor-boundary
      // timing race rather than a full ring overflow.
      {MP_QSTR_dma_buf_count, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 16}},
      {MP_QSTR_dma_buf_len, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 1024}},
      {MP_QSTR_i2c_shared, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false}},
  };
  mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)];
  mp_arg_parse_all_kw_array(n_args, n_kw, args, MP_ARRAY_SIZE(allowed_args),
                            allowed_args, parsed);

  audiorecorder_obj_t *self = mp_obj_malloc(audiorecorder_obj_t, type);
  self->port_num = (i2s_port_t)parsed[ARG_i2s_num].u_int;
  self->i2c_port = (i2c_port_t)parsed[ARG_i2c_port].u_int;
  self->sample_rate = parsed[ARG_sample_rate].u_int;
  self->channels = parsed[ARG_channels].u_int;
  self->task = NULL;
  self->stack = NULL;
  self->recording = false;
  self->stop_request = false;
  self->last_error = 0;
  self->total_bytes = 0;
  self->max_bytes = 0;
  self->last_peak = 0;
  self->i2s_error_count = 0;
  self->rb_full_stalls = 0;
  self->file_obj = mp_const_none;
  self->drain_pending = false;
  self->i2s_installed = false;
  self->i2c_installed = false;
  self->codec = NULL;
  self->rx_handle = NULL;
  self->done_sem = xSemaphoreCreateBinary();

  if (!rb_init(&self->rb, RECORD_RING_BUF_SIZE)) {
    mp_raise_OSError(MP_ENOMEM);
  }

  // ---- I2C bus, for configuring the ES7210 -----------------------------
  // The es7210 component takes a plain i2c_port_t and drives it with the
  // legacy I2C driver's transaction calls internally, so that's what we
  // bring the bus up with here too. If your ESP-IDF version has dropped
  // driver/i2c.h entirely, you'll need Espressif's newer
  // "espressif/esp_codec_dev" component instead (see README.md).
  //
  // i2c_shared=True skips installing our own driver on i2c_port entirely
  // -- for boards like the T-Deck where that bus is already brought up
  // by something else (a touchscreen driver, your own code, etc.) and
  // installing it again here would just fail. We still don't touch
  // machine.I2C directly either way: if it's what already installed the
  // port, make sure it's using the legacy driver/i2c.h backend, since
  // that's what es7210_i2c_config_t expects to talk to.
  bool i2c_shared = parsed[ARG_i2c_shared].u_bool;
  esp_err_t ret = ESP_OK;
  if (!i2c_shared) {
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = parsed[ARG_i2c_sda].u_int,
        .scl_io_num = parsed[ARG_i2c_scl].u_int,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    ret = i2c_param_config(self->i2c_port, &i2c_conf);
    if (ret == ESP_OK) {
      ret = i2c_driver_install(self->i2c_port, I2C_MODE_MASTER, 0, 0, 0);
    }
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "I2C bus init on port %d (sda=%d scl=%d) failed: %s",
               self->i2c_port, parsed[ARG_i2c_sda].u_int,
               parsed[ARG_i2c_scl].u_int, esp_err_to_name(ret));
      log_err_to_console("I2C bus init on port %d (sda=%d scl=%d) failed: %s",
                         self->i2c_port, parsed[ARG_i2c_sda].u_int,
                         parsed[ARG_i2c_scl].u_int, esp_err_to_name(ret));
      rb_deinit(&self->rb);
      mp_raise_OSError(MP_EIO);
    }
    self->i2c_installed = true;
  }

  // ---- ES7210 codec (I2C control path only) ----------------------------
  es7210_i2c_config_t es_i2c_conf = {
      .i2c_port = self->i2c_port,
      .i2c_addr = (uint8_t)parsed[ARG_i2c_addr].u_int,
  };
  ret = es7210_new_codec(&es_i2c_conf, &self->codec);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "es7210_new_codec (i2c_addr=0x%02x) failed: %s",
             (unsigned)es_i2c_conf.i2c_addr, esp_err_to_name(ret));
    log_err_to_console("es7210_new_codec (i2c_addr=0x%02x) failed: %s",
                       (unsigned)es_i2c_conf.i2c_addr, esp_err_to_name(ret));
    if (self->i2c_installed) {
      i2c_driver_delete(self->i2c_port);
    }
    rb_deinit(&self->rb);
    mp_raise_OSError(MP_EIO);
  }

  es7210_codec_config_t codec_conf = {
      .sample_rate_hz = (uint32_t)self->sample_rate,
      .mclk_ratio = (uint32_t)parsed[ARG_mclk_ratio].u_int,
      .i2s_format = ES7210_I2S_FMT_I2S,
      .bit_width = ES7210_I2S_BITS_16B,
      .mic_bias = (es7210_mic_bias_t)parsed[ARG_mic_bias].u_int,
      .mic_gain = (es7210_mic_gain_t)parsed[ARG_mic_gain].u_int,
      .flags = {.tdm_enable = 0},
  };
  ret = es7210_config_codec(self->codec, &codec_conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG,
             "es7210_config_codec failed: %s -- check i2c_addr, wiring, "
             "and that nothing else already claimed this I2C port",
             esp_err_to_name(ret));
    log_err_to_console("es7210_config_codec failed: %s -- check i2c_addr, "
                       "wiring, and that nothing else already claimed this "
                       "I2C port",
                       esp_err_to_name(ret));
    es7210_del_codec(self->codec);
    if (self->i2c_installed) {
      i2c_driver_delete(self->i2c_port);
    }
    rb_deinit(&self->rb);
    mp_raise_OSError(MP_EIO);
  }

  // ---- I2S RX (the actual audio data path) ------------------------------
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(self->port_num, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = parsed[ARG_dma_buf_count].u_int;
  chan_cfg.dma_frame_num = parsed[ARG_dma_buf_len].u_int;
  ret =
      i2s_new_channel(&chan_cfg, NULL, &self->rx_handle); // NULL tx -- RX only

  if (ret == ESP_OK) {
    // Always receive STEREO here, regardless of self->channels. The
    // ES7210 always clocks out both slots of the active mic pair over
    // the wire -- it has no "mono transmit" mode -- so if the I2S RX
    // side is instead configured for I2S_SLOT_MODE_MONO when
    // channels=1, it ends up expecting half as many bit-clocks per
    // frame as the codec is actually sending, and every sample decodes
    // misaligned. That's "mostly noise": the hardware was slot-mismatched
    // with what's on the wire, not a gain/config problem in the codec
    // itself. When the caller wants mono output, audio_record_task()
    // downmixes the correctly-received stereo stream in software instead
    // (see the extraction there) -- receive at the real wire format,
    // reshape to what the caller asked for afterwards.
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)self->sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = parsed[ARG_mclk].u_int,
                .bclk = parsed[ARG_bck].u_int,
                .ws = parsed[ARG_ws].u_int,
                .dout = I2S_GPIO_UNUSED,
                .din = parsed[ARG_din].u_int,
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv = false,
                    },
            },
    };
    ret = i2s_channel_init_std_mode(self->rx_handle, &std_cfg);
  }
  if (ret == ESP_OK) {
    ret = i2s_channel_enable(self->rx_handle);
  }
  if (ret != ESP_OK) {
    ESP_LOGE(TAG,
             "I2S RX setup (i2s_num=%d mclk=%d bck=%d ws=%d din=%d) "
             "failed: %s",
             self->port_num, parsed[ARG_mclk].u_int, parsed[ARG_bck].u_int,
             parsed[ARG_ws].u_int, parsed[ARG_din].u_int, esp_err_to_name(ret));
    log_err_to_console("I2S RX setup (i2s_num=%d mclk=%d bck=%d ws=%d din=%d) "
                       "failed: %s",
                       self->port_num, parsed[ARG_mclk].u_int,
                       parsed[ARG_bck].u_int, parsed[ARG_ws].u_int,
                       parsed[ARG_din].u_int, esp_err_to_name(ret));
    if (self->rx_handle) {
      i2s_del_channel(self->rx_handle);
    }
    es7210_del_codec(self->codec);
    if (self->i2c_installed) {
      i2c_driver_delete(self->i2c_port);
    }
    rb_deinit(&self->rb);
    mp_raise_OSError(MP_EIO);
  }
  self->i2s_installed = true;

  return MP_OBJ_FROM_PTR(self);
}

// record(path, seconds=None) -- seconds is an optional auto-stop limit;
// omit it (or pass None) to record until stop() is called.
static mp_obj_t audiorecorder_record(size_t n_args, const mp_obj_t *pos_args,
                                     mp_map_t *kw_args) {
  enum { ARG_seconds };
  static const mp_arg_t allowed_args[] = {
      {MP_QSTR_seconds, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none}},
  };
  mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)];
  mp_arg_parse_all(n_args - 2, pos_args + 2, kw_args,
                   MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);

  audiorecorder_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
  mp_obj_t path_obj = pos_args[1];

  if (self->recording) {
    mp_raise_msg(&mp_type_RuntimeError,
                 MP_ERROR_TEXT("already recording; call stop() first"));
  }

  // Reclaim the previous recording's PSRAM stack if record() is being
  // called again without an intervening stop() (e.g. it hit its own
  // seconds= limit). self->recording == false (just confirmed above)
  // means audio_record_task() already reached its own "self->recording =
  // false; xSemaphoreGive(done_sem)" exit sequence -- take done_sem
  // (returns immediately, since it's already given) to synchronize with
  // that before freeing the stack out from under it, same reasoning as
  // audiorecorder_stop()'s identical take just below.
  if (self->stack != NULL) {
    xSemaphoreTake(self->done_sem, pdMS_TO_TICKS(2000));
    heap_caps_free(self->stack);
    self->stack = NULL;
    self->task = NULL;
  }

  // VFS-aware, same reasoning as audioplayer.c's play(): works with
  // anything MicroPython has mounted, and a bad path raises a normal
  // Python OSError.
  mp_obj_t open_fn = mp_load_global(MP_QSTR_open);
  mp_obj_t open_args[2] = {path_obj, MP_OBJ_NEW_QSTR(MP_QSTR_wb)};
  mp_obj_t file = mp_call_function_n_kw(open_fn, 2, 0, open_args);
  self->file_obj = file;

  // Placeholder header (data size 0) -- see the file-header comment on
  // why this matters if stop() never gets called.
  uint8_t hdr[WAV_HEADER_SIZE];
  build_wav_header(hdr, self->sample_rate, self->channels, 0);
  int errcode = 0;
  const mp_stream_p_t *stream_p =
      mp_get_stream_raise(self->file_obj, MP_STREAM_OP_WRITE);
  stream_p->write(self->file_obj, hdr, WAV_HEADER_SIZE, &errcode);

  self->total_bytes = 0;
  self->max_bytes = 0;
  self->i2s_error_count = 0;
  self->rb_full_stalls = 0;
  if (parsed[ARG_seconds].u_obj != mp_const_none) {
    mp_float_t secs = mp_obj_get_float(parsed[ARG_seconds].u_obj);
    mp_float_t bytes_per_sec =
        (mp_float_t)self->sample_rate * self->channels * 2;
    if (secs > 0) {
      self->max_bytes = (uint32_t)(secs * bytes_per_sec);
    }
  }

  rb_reset(&self->rb);
  self->stop_request = false;
  self->last_error = 0;
  self->recording = true;

  // PSRAM-backed stack -- see modules/modssh/modssh.c's ssh_client_connect()
  // for the full rationale (same pattern).
  StackType_t *stack = heap_caps_malloc(RECORD_TASK_STACK_WORDS * sizeof(StackType_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (stack == NULL) {
    stack = heap_caps_malloc(RECORD_TASK_STACK_WORDS * sizeof(StackType_t),
                             MALLOC_CAP_8BIT);
  }
  if (stack == NULL) {
    self->recording = false;
    mp_raise_msg(&mp_type_RuntimeError,
                 MP_ERROR_TEXT("failed to allocate recording task stack"));
  }
  self->stack = stack;

  TaskHandle_t task = xTaskCreateStaticPinnedToCore(
      audio_record_task, "audiorecorder", RECORD_TASK_STACK_WORDS, self,
      RECORD_TASK_PRIORITY, stack, &self->task_buf,
      1 /* APP CPU, same as audioplayer.c */);
  if (task == NULL) {
    heap_caps_free(stack);
    self->stack = NULL;
    self->recording = false;
    mp_raise_msg(&mp_type_RuntimeError,
                 MP_ERROR_TEXT("failed to start recording task"));
  }
  self->task = task;

  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audiorecorder_record_obj, 2,
                                  audiorecorder_record);

// Drains any remaining buffered audio and patches the WAV header with the
// real data size. Safe to call whether the task is still running (it'll
// be stopped first), already finished on its own (record(seconds=...)
// hit its limit), or there's nothing to finalize at all.
static void audiorecorder_finalize(audiorecorder_obj_t *self) {
  if (self->file_obj == mp_const_none) {
    return;
  }

  while (audiorecorder_drain_internal(self) > 0) {
    // keep draining until the ring buffer reports empty
  }

  mp_obj_t seek_dest[4];
  mp_load_method(self->file_obj, MP_QSTR_seek, seek_dest);
  seek_dest[2] = mp_obj_new_int(0);
  seek_dest[3] = mp_obj_new_int(0);
  mp_call_method_n_kw(2, 0, seek_dest);

  uint8_t hdr[WAV_HEADER_SIZE];
  build_wav_header(hdr, self->sample_rate, self->channels, self->total_bytes);
  int errcode = 0;
  const mp_stream_p_t *stream_p =
      mp_get_stream_raise(self->file_obj, MP_STREAM_OP_WRITE);
  stream_p->write(self->file_obj, hdr, WAV_HEADER_SIZE, &errcode);

  mp_obj_t close_meth[2];
  mp_load_method(self->file_obj, MP_QSTR_close, close_meth);
  mp_call_method_n_kw(0, 0, close_meth);
  self->file_obj = mp_const_none;
}

static mp_obj_t audiorecorder_stop(mp_obj_t self_in) {
  audiorecorder_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (self->task != NULL) {
    self->stop_request = true;
    // Only free the PSRAM stack if the task actually confirmed exit --
    // on a timeout it may still be running on it.
    if (xSemaphoreTake(self->done_sem, pdMS_TO_TICKS(2000)) == pdTRUE &&
        self->stack != NULL) {
      heap_caps_free(self->stack);
      self->stack = NULL;
    }
    self->task = NULL;
  }
  audiorecorder_finalize(self);
  return mp_obj_new_int(self->total_bytes);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiorecorder_stop_obj, audiorecorder_stop);

static mp_obj_t audiorecorder_is_recording(mp_obj_t self_in) {
  audiorecorder_obj_t *self = MP_OBJ_TO_PTR(self_in);
  return mp_obj_new_bool(self->recording);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiorecorder_is_recording_obj,
                                 audiorecorder_is_recording);

// Peak sample magnitude (0-32767) from the most recently captured chunk.
// Cheap VU-meter-style level, not a speech/silence classifier -- see the
// file-header comment for the VAD caveat.
static mp_obj_t audiorecorder_level(mp_obj_t self_in) {
  audiorecorder_obj_t *self = MP_OBJ_TO_PTR(self_in);
  return mp_obj_new_int(self->last_peak);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiorecorder_level_obj, audiorecorder_level);

// (peak, total_bytes, i2s_error_count, rb_full_stalls). Meant to be
// polled from ordinary Python code during/after a recording -- e.g.
// `while rec.is_recording(): print(rec.diagnostics())` -- rather than
// printed from C. The capture task only ever increments plain volatile
// counters on `self`; it never calls into MicroPython's stdout/print
// machinery itself, since that's not guaranteed safe to touch from a
// task the interpreter doesn't know about (same reasoning as why the
// task never touches an mp_obj_t directly elsewhere in this file). This
// is how to get the i2s_error_count/rb_full_stalls numbers (also logged
// via ESP_LOGW/ESP_LOGI, for anyone who *can* run idf.py monitor)
// without needing that tool.
static mp_obj_t audiorecorder_diagnostics(mp_obj_t self_in) {
  audiorecorder_obj_t *self = MP_OBJ_TO_PTR(self_in);
  mp_obj_t items[4] = {
      mp_obj_new_int(self->last_peak),
      mp_obj_new_int(self->total_bytes),
      mp_obj_new_int(self->i2s_error_count),
      mp_obj_new_int(self->rb_full_stalls),
  };
  return mp_obj_new_tuple(4, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiorecorder_diagnostics_obj,
                                 audiorecorder_diagnostics);

static mp_obj_t audiorecorder_last_error(mp_obj_t self_in) {
  audiorecorder_obj_t *self = MP_OBJ_TO_PTR(self_in);
  return mp_obj_new_int(self->last_error);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiorecorder_last_error_obj,
                                 audiorecorder_last_error);

static mp_obj_t audiorecorder_deinit(mp_obj_t self_in) {
  audiorecorder_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (self->task != NULL) {
    self->stop_request = true;
    // Only free the PSRAM stack if the task actually confirmed exit --
    // on a timeout it may still be running on it.
    if (xSemaphoreTake(self->done_sem, pdMS_TO_TICKS(2000)) == pdTRUE &&
        self->stack != NULL) {
      heap_caps_free(self->stack);
      self->stack = NULL;
    }
    self->task = NULL;
  }
  audiorecorder_finalize(self);
  rb_deinit(&self->rb);

  if (self->i2s_installed) {
    i2s_channel_disable(self->rx_handle);
    i2s_del_channel(self->rx_handle);
    self->rx_handle = NULL;
    self->i2s_installed = false;
  }
  if (self->codec != NULL) {
    es7210_del_codec(self->codec);
    self->codec = NULL;
  }
  if (self->i2c_installed) {
    i2c_driver_delete(self->i2c_port);
    self->i2c_installed = false;
  }
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiorecorder_deinit_obj,
                                 audiorecorder_deinit);

static const mp_rom_map_elem_t audiorecorder_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_record), MP_ROM_PTR(&audiorecorder_record_obj)},
    {MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&audiorecorder_stop_obj)},
    {MP_ROM_QSTR(MP_QSTR_is_recording),
     MP_ROM_PTR(&audiorecorder_is_recording_obj)},
    {MP_ROM_QSTR(MP_QSTR_level), MP_ROM_PTR(&audiorecorder_level_obj)},
    {MP_ROM_QSTR(MP_QSTR_diagnostics),
     MP_ROM_PTR(&audiorecorder_diagnostics_obj)},
    {MP_ROM_QSTR(MP_QSTR_last_error),
     MP_ROM_PTR(&audiorecorder_last_error_obj)},
    {MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiorecorder_deinit_obj)},
};
static MP_DEFINE_CONST_DICT(audiorecorder_locals_dict,
                            audiorecorder_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(audiorecorder_AudioRecorder_type,
                         MP_QSTR_AudioRecorder, MP_TYPE_FLAG_NONE, make_new,
                         audiorecorder_make_new, locals_dict,
                         &audiorecorder_locals_dict);

// No MP_REGISTER_MODULE here -- this type is registered into the
// existing `audioplayer` module's globals table in audioplayer.c.
