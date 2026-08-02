/*
 * MicroPython Audio Playing Interface Library
 * Copyright (c) 2026 8bitmcu
 * License: MIT
 *
 * Decoding is done with the fixed-point Helix MP3 decoder, wrapped by
 * the "esp-libhelix-mp3" ESP-IDF component:
 * https://github.com/chmorgan/esp-libhelix-mp3
 *
 * Playback runs in its own FreeRTOS task so that Python code is free to
 * keep running while a file plays.
 *
 * VFS awareness: the decode task never opens or reads the file itself.
 * MicroPython's mounted filesystems (internal flash lfs2/FAT, SD cards
 * mounted via os.mount(), etc.) are implemented at the MicroPython VFS
 * level, not registered with ESP-IDF/newlib's POSIX VFS -- so a raw
 * fopen()/fread() from a plain FreeRTOS task generally can't see them at
 * all (this is why earlier versions of this module could compile and run
 * but never actually produced sound: fopen() was silently failing).
 *
 * Instead, the file is opened with MicroPython's own open() from the
 * *main* MicroPython thread (in play()/feed()), and its bytes are pushed
 * into a small thread-safe ring buffer. The background task only ever
 * touches that ring buffer, the decoder, and the I2S driver -- never an
 * mp_obj_t -- so it stays safe to run outside the interpreter thread.
 * When the ring buffer runs low, the task uses mp_sched_schedule() to ask
 * the main thread to top it back up at its next safe point, so playback
 * is normally hands-off; a feed() method is also exposed for manual use.
 */

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
// xTaskCreatePinnedToCore moved here in ESP-IDF 5.3+ (upstream FreeRTOS SMP
// merge); harmless to include on older IDF versions where it's a no-op/absent.
#if __has_include("freertos/idf_additions.h")
#include "freertos/idf_additions.h"
#endif

#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "mp3dec.h" // from the esp-libhelix-mp3 component

#include "ring_buf.h" // shared with audioplayer.c

static const char *TAG = "audioplayer";

// ---- tunables -------------------------------------------------------

#define AUDIO_READ_BUF_SIZE (4 * 1024) // raw mp3 bytes kept in flight
#define AUDIO_REFILL_THRESHOLD                                                 \
  (2 * 1024) // refill file data once buffer drops below this
#define AUDIO_MAX_FRAME_SAMPS 1152  // per Helix, per channel
#define AUDIO_TASK_STACK_WORDS 6144 // FreeRTOS task stack, in words (~24KB)
#define AUDIO_TASK_PRIORITY 5

#define RING_BUF_SIZE (24 * 1024) // bytes of compressed mp3 data kept queued
#define FEED_CHUNK_SIZE 2048      // bytes read from the VFS stream per feed
#define FEED_PRIME_ROUNDS 8       // feed calls done synchronously by play()

typedef struct _audioplayer_obj_t {
  mp_obj_base_t base;

  i2s_port_t port_num;         // Kept for reference
  i2s_chan_handle_t tx_handle; // <-- New I2S channel handle

  // task control
  TaskHandle_t task;
  StaticTask_t task_buf; // xTaskCreateStaticPinnedToCore()'s control block
  StackType_t *stack;    // PSRAM-backed, see audioplayer_play()
  SemaphoreHandle_t done_sem;
  volatile bool playing;
  volatile bool stop_request;
  volatile bool paused;
  volatile int last_error; // 0 = ok, else a Helix / file error code
  volatile int last_samprate;
  volatile int last_channels;

  volatile int volume_pct; // 0-100, applied to PCM before i2s_write

  // VFS-backed source: only ever touched from the MicroPython thread
  // (play() / feed() / stop() / deinit()), never from audio_play_task().
  mp_obj_t file_obj;
  volatile bool feed_pending; // a feed callback is already scheduled

  ring_buf_t rb;

  char id3_title[128];
  char id3_artist[128];
  char id3_album[128];

  volatile int bitrate_kbps;
  volatile int duration_seconds;

  bool i2s_installed;
} audioplayer_obj_t;

const mp_obj_type_t audioplayer_AudioPlayer_type;

extern const mp_obj_type_t audiorecorder_AudioRecorder_type;

// Basic WAV header parser. Looks for 'fmt ' and 'data' chunks.
// Returns the byte offset to the start of the audio data, or -1 if invalid.
static int parse_wav_header(const uint8_t *data, size_t len, int *channels,
                            int *sample_rate, int *bps, uint32_t *data_bytes) {
  if (len < 44)
    return -1;
  if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0)
    return -1;

  size_t offset = 12;
  bool found_fmt = false;

  // Scan chunks until we find 'data'
  while (offset + 8 <= len) {
    uint32_t chunk_sz = data[offset + 4] | (data[offset + 5] << 8) |
                        (data[offset + 6] << 16) | (data[offset + 7] << 24);

    if (memcmp(data + offset, "fmt ", 4) == 0) {
      if (chunk_sz < 16)
        return -1;
      int format = data[offset + 8] | (data[offset + 9] << 8);
      if (format != 1)
        return -1; // Only uncompressed PCM supported

      *channels = data[offset + 10] | (data[offset + 11] << 8);
      *sample_rate = data[offset + 12] | (data[offset + 13] << 8) |
                     (data[offset + 14] << 16) | (data[offset + 15] << 24);
      *bps = data[offset + 22] | (data[offset + 23] << 8);
      found_fmt = true;
    } else if (memcmp(data + offset, "data", 4) == 0) {
      if (found_fmt) {
        *data_bytes = chunk_sz;
        return offset + 8;
      }
    }

    offset += 8 + chunk_sz;
  }
  return -1;
}

// Best-effort extraction of ID3 strings, safely stripping nulls and
// non-printables
static void extract_id3_string(const uint8_t *data, uint32_t len, char *dest,
                               size_t dest_size) {
  if (len <= 1 || dest_size == 0)
    return;
  uint32_t src_pos = 1; // Skip the text encoding byte
  uint32_t dst_pos = 0;

  // Skip UTF-16 BOM if present
  if (len >= 3 && ((data[1] == 0xFF && data[2] == 0xFE) ||
                   (data[1] == 0xFE && data[2] == 0xFF))) {
    src_pos += 2;
  }

  while (src_pos < len && dst_pos < dest_size - 1) {
    uint8_t c = data[src_pos++];
    // Keep printable ASCII and valid UTF-8 bytes; ignore null-padding
    if (c >= 32) {
      dest[dst_pos++] = c;
    }
  }
  dest[dst_pos] = '\0';
}

// ---- helpers ----------------------------------------------------------

// Scale 16-bit signed PCM samples in place by volume_pct/100, with clipping.
static void apply_volume(int16_t *buf, int n_samples, int volume_pct) {
  if (volume_pct >= 100)
    return;
  if (volume_pct <= 0) {
    memset(buf, 0, n_samples * sizeof(int16_t));
    return;
  }

  // Pre-calculate scaling factor (Q15 fixed point)
  // volume_pct is 0-100. We want to scale by (pct / 100).
  // In Q15, 1.0 is 32768. So we scale by (volume_pct * 327)
  int32_t scale = (volume_pct * 327);

  for (int i = 0; i < n_samples; i++) {
    // Perform multiplication and shift instead of division
    int32_t v = ((int32_t)buf[i] * scale) >> 15;

    // Manual clamping (usually faster than conditional logic if compiler
    // doesn't vectorize)
    if (v > 32767)
      v = 32767;
    else if (v < -32768)
      v = -32768;

    buf[i] = (int16_t)v;
  }
}
// ---- Duration probing ----------------------------------------------------
//
// None of this touches I2S, the decode task, or the ring buffer -- it's
// plain synchronous VFS + CPU work run straight on the calling (Python)
// thread, same as feed(). Three techniques, cheapest/least-reliable
// first:
//
//   1. Xing/Info (LAME) or VBRI (Fraunhofer) header in the first frame,
//      if the encoder wrote one: gives an exact total frame count
//      directly, for the cost of reading a few KB.
//   2. If neither is present: estimate from (file size / bitrate of the
//      first frame), assuming constant bitrate. Wrong for true VBR
//      files with no VBR header (rare, but it happens with older/basic
//      encoders), right for everything else.
//   3. exact=True, or as an automatic fallback if #2 has nothing to
//      work with (e.g. free-format bitrate): walk every frame header
//      in the file and sum samples. Exact regardless of encoding, but
//      touches the whole file -- still much cheaper than actually
//      decoding it, since this only parses headers (MP3GetNextFrameInfo
//      does no Huffman decoding).

#define DURATION_PROBE_BUF (8 * 1024)

// Layer III frame length in bytes. Not something Helix's public API
// exposes -- this is the standard MPEG formula, needed to step from one
// frame header to the next without actually decoding the frame.
static int mp3_frame_length(int bitrate, int samprate, bool mpeg1,
                            int padding) {
  if (bitrate <= 0 || samprate <= 0) {
    return 0;
  }
  int coeff = mpeg1 ? 144 : 72;
  return (coeff * bitrate) / samprate + padding;
}

// Looks for a Xing/Info or VBRI header inside the first frame at `frame`
// (frame_avail bytes available from there). On success returns 0 and
// fills *out_num_frames; returns -1 if neither is present/usable.
static int find_vbr_header(const uint8_t *frame, size_t frame_avail,
                           const MP3FrameInfo *info, uint32_t *out_num_frames) {
  bool mono = (info->nChans == 1);
  bool mpeg1 = (info->version == 0); // MPEGVersion: MPEG1=0, MPEG2=1, MPEG25=2
  size_t side_info = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
  size_t xing_off = 4 + side_info;

  if (xing_off + 8 <= frame_avail &&
      (memcmp(frame + xing_off, "Xing", 4) == 0 ||
       memcmp(frame + xing_off, "Info", 4) == 0)) {
    uint32_t flags = ((uint32_t)frame[xing_off + 4] << 24) |
                     ((uint32_t)frame[xing_off + 5] << 16) |
                     ((uint32_t)frame[xing_off + 6] << 8) | frame[xing_off + 7];
    if ((flags & 0x1) && xing_off + 12 <= frame_avail) {
      *out_num_frames = ((uint32_t)frame[xing_off + 8] << 24) |
                        ((uint32_t)frame[xing_off + 9] << 16) |
                        ((uint32_t)frame[xing_off + 10] << 8) |
                        frame[xing_off + 11];
      return 0;
    }
    return -1; // Xing/Info present but didn't encode a frame count
  }

  size_t vbri_off = 4 + 32; // VBRI is always here, version/channels aside
  if (vbri_off + 18 <= frame_avail &&
      memcmp(frame + vbri_off, "VBRI", 4) == 0) {
    *out_num_frames = ((uint32_t)frame[vbri_off + 14] << 24) |
                      ((uint32_t)frame[vbri_off + 15] << 16) |
                      ((uint32_t)frame[vbri_off + 16] << 8) |
                      frame[vbri_off + 17];
    return 0;
  }

  return -1;
}

// Reads more data onto the end of buf[0..bytes_left). Returns the new
// bytes_left; unchanged from the input means EOF/error (no progress).
static size_t duration_refill(mp_obj_t file, const mp_stream_p_t *stream_p,
                              uint8_t *buf, size_t bytes_left,
                              size_t buf_size) {
  if (bytes_left >= buf_size) {
    return bytes_left;
  }
  int errcode = 0;
  mp_uint_t n =
      stream_p->read(file, buf + bytes_left, buf_size - bytes_left, &errcode);
  if (n == MP_STREAM_ERROR || n == 0) {
    return bytes_left;
  }
  return bytes_left + n;
}

// Walks every MP3 frame header from buf[0..bytes_left) to EOF, summing
// samples. buf/buf_size is reused as the read window throughout. Returns
// duration in seconds, or -1.0 if no valid frame could be found at all.
static double mp3_duration_full_scan(mp_obj_t file, uint8_t *buf,
                                     size_t buf_size, size_t bytes_left) {
  const mp_stream_p_t *stream_p = mp_get_stream_raise(file, MP_STREAM_OP_READ);
  HMP3Decoder dec = MP3InitDecoder();
  uint64_t total_samples_per_channel = 0;
  int samprate = 0;

  while (true) {
    if (bytes_left < 16) {
      size_t before = bytes_left;
      bytes_left = duration_refill(file, stream_p, buf, bytes_left, buf_size);
      if (bytes_left == before && bytes_left < 4) {
        break; // truly out of data
      }
    }

    int offset = MP3FindSyncWord(buf, bytes_left);
    if (offset < 0) {
      size_t before = bytes_left;
      bytes_left = duration_refill(file, stream_p, buf, bytes_left, buf_size);
      if (bytes_left == before) {
        break; // no more data and no sync word in what we have -- done
      }
      continue;
    }

    if (bytes_left - (size_t)offset < 16) {
      memmove(buf, buf + offset, bytes_left - offset);
      bytes_left -= offset;
      size_t before = bytes_left;
      bytes_left = duration_refill(file, stream_p, buf, bytes_left, buf_size);
      if (bytes_left == before) {
        break; // header straddles EOF; nothing more to count
      }
      continue;
    }

    MP3FrameInfo info;
    int err = MP3GetNextFrameInfo(dec, &info, buf + offset);
    int padding = (buf[offset + 2] >> 1) & 0x1;
    int frame_len = (err == 0) ? mp3_frame_length(info.bitrate, info.samprate,
                                                  info.version == 0, padding)
                               : 0;

    if (err != 0 || frame_len < 4 || info.samprate <= 0) {
      // False sync -- nudge forward a byte and keep looking, same
      // defensive resync used by playback.
      memmove(buf, buf + offset + 1, bytes_left - offset - 1);
      bytes_left -= (offset + 1);
      continue;
    }

    if ((size_t)frame_len > buf_size) {
      break; // pathological; bail rather than loop forever
    }

    if (bytes_left - (size_t)offset < (size_t)frame_len) {
      memmove(buf, buf + offset, bytes_left - offset);
      bytes_left -= offset;
      size_t before = bytes_left;
      bytes_left = duration_refill(file, stream_p, buf, bytes_left, buf_size);
      if (bytes_left == before) {
        break; // truncated final frame; stop counting here
      }
      continue;
    }

    total_samples_per_channel +=
        info.outputSamps / (info.nChans > 0 ? info.nChans : 1);
    samprate = info.samprate;

    size_t consumed = (size_t)offset + (size_t)frame_len;
    memmove(buf, buf + consumed, bytes_left - consumed);
    bytes_left -= consumed;
  }

  MP3FreeDecoder(dec);
  if (samprate <= 0) {
    return -1.0;
  }
  return (double)total_samples_per_channel / samprate;
}

// mp_load_method()/mp_call_method_n_kw() plumbing for file.seek(offset,
// whence), used to get the file size (seek to end, tell via return
// value, seek back) without depending on os.stat() being meaningful for
// this particular VFS (some custom/streaming VFS implementations, e.g.
// a network-backed one, may not report a size any other way).
static mp_int_t duration_stream_seek(mp_obj_t file, mp_int_t offset,
                                     mp_int_t whence) {
  mp_obj_t dest[4];
  mp_load_method(file, MP_QSTR_seek, dest);
  dest[2] = mp_obj_new_int(offset);
  dest[3] = mp_obj_new_int(whence);
  mp_obj_t res = mp_call_method_n_kw(2, 0, dest);
  return mp_obj_get_int(res);
}

// ---- VFS feed (runs on the MicroPython thread only) ---------------------
//
// Pulls up to FEED_CHUNK_SIZE bytes from self->file_obj (any MicroPython
// stream: internal flash, SD card, a live network socket, etc.) into the
// ring buffer. Safe to call repeatedly; a no-op once EOF has been reached.
//
// Returns: 1 = queued more data, 0 = EOF / nothing to do, -1 = no file open.
static int audioplayer_feed_internal(audioplayer_obj_t *self) {
  if (self->file_obj == mp_const_none) {
    return -1;
  }
  if (self->rb.eof) {
    return 0;
  }

  size_t space = rb_free_space(&self->rb);
  if (space == 0) {
    return 1; // buffer's already full, nothing to do right now
  }

  uint8_t chunk[FEED_CHUNK_SIZE];
  size_t to_read = (space < sizeof(chunk)) ? space : sizeof(chunk);

  int errcode = 0;
  const mp_stream_p_t *stream_p =
      mp_get_stream_raise(self->file_obj, MP_STREAM_OP_READ);
  mp_uint_t n = stream_p->read(self->file_obj, chunk, to_read, &errcode);

  if (errcode == MP_EAGAIN || errcode == MP_ETIMEDOUT) {
    // Transient: a live network socket (radio streaming) armed with
    // settimeout() simply had no data ready within its read timeout --
    // the connection is still alive, this is NOT end of stream. Leave
    // rb.eof false and file_obj open; report "nothing queued this
    // round" the same as an ordinary empty read, so the decode task's
    // own request_feed()/vTaskDelay(10ms) retry loop just tries again
    // next time instead of tearing down playback.
    return 0;
  }

  if (errcode != 0) {
    self->last_error = -errcode;
    n = 0;
  }

  if (n == 0 || n == MP_STREAM_ERROR) {
    self->rb.eof = true;
    // We're done with the file now -- close it here, on the MP thread.
    mp_obj_t close_meth[2];
    mp_load_method(self->file_obj, MP_QSTR_close, close_meth);
    mp_call_method_n_kw(0, 0, close_meth);
    self->file_obj = mp_const_none;
    return 0;
  }

  rb_write(&self->rb, chunk, n);
  return 1;
}

// mp_sched_schedule callback: the decode task asks for this to run on the
// main thread the next time the interpreter is at a safe point. Feeds
// several chunks at once so we don't need to be re-scheduled constantly.
static mp_obj_t audioplayer_feed_scheduled(mp_obj_t self_in) {
  audioplayer_obj_t *self = MP_OBJ_TO_PTR(self_in);
  self->feed_pending = false;
  for (int i = 0; i < FEED_PRIME_ROUNDS; i++) {
    if (audioplayer_feed_internal(self) <= 0) {
      break;
    }
  }
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioplayer_feed_scheduled_obj,
                                 audioplayer_feed_scheduled);

// Called from the decode task when the ring buffer is running low. Cheap
// and safe to call from another task/ISR by design.
static void audioplayer_request_feed(audioplayer_obj_t *self) {
  if (!self->feed_pending) {
    self->feed_pending = true;
    if (!mp_sched_schedule(MP_OBJ_FROM_PTR(&audioplayer_feed_scheduled_obj),
                           MP_OBJ_FROM_PTR(self))) {
      self->feed_pending =
          false; // scheduler queue was full; try again next time
    }
  }
}

// ---- playback task ------------------------------------------------------

static void audio_play_task(void *arg) {
  audioplayer_obj_t *self = (audioplayer_obj_t *)arg;

  self->last_samprate = 0;
  self->last_channels = 0;

  uint8_t *read_buf = heap_caps_malloc(AUDIO_READ_BUF_SIZE,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (read_buf == NULL) {
    read_buf = heap_caps_malloc(AUDIO_READ_BUF_SIZE, MALLOC_CAP_8BIT);
  }
  if (read_buf == NULL) {
    ESP_LOGE(TAG, "out of memory allocating read buffer");
    self->last_error = -MP_ENOMEM;
    goto done_no_buf;
  }

  // 1. Buffer the first chunk of the file to determine the format
  int bytes_left = 0;
  while (!self->stop_request && bytes_left < 256) {
    if (self->rb.eof && rb_available(&self->rb) == 0)
      break;
    size_t n = rb_read(&self->rb, read_buf + bytes_left, 256 - bytes_left);
    bytes_left += n;
    if (n == 0) {
      audioplayer_request_feed(self);
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }

  if (bytes_left == 0 || self->stop_request) {
    goto done;
  }

  // 2. Format Detection Branch
  if (bytes_left >= 12 && memcmp(read_buf, "RIFF", 4) == 0 &&
      memcmp(read_buf + 8, "WAVE", 4) == 0) {
    ESP_LOGI(TAG, "Detected WAV format");
    int channels = 0, sample_rate = 0, bps = 0;
    uint32_t data_bytes = 0;
    int data_offset = -1;

    // Fetch more bytes if the WAV header contains long metadata chunks
    while (!self->stop_request && bytes_left < AUDIO_READ_BUF_SIZE) {
      data_offset = parse_wav_header(read_buf, bytes_left, &channels,
                                     &sample_rate, &bps, &data_bytes);
      if (data_offset > 0)
        break;

      if (self->rb.eof && rb_available(&self->rb) == 0)
        break;
      size_t n = rb_read(&self->rb, read_buf + bytes_left,
                         AUDIO_READ_BUF_SIZE - bytes_left);
      bytes_left += n;
      if (n == 0) {
        audioplayer_request_feed(self);
        vTaskDelay(pdMS_TO_TICKS(10));
      }
    }

    if (data_offset < 0 || bps != 16) {
      ESP_LOGE(
          TAG,
          "Unsupported WAV format or missing data chunk (Must be 16-bit PCM)");
      self->last_error = -MP_EINVAL;
      goto done;
    }

    // bits per second = sample_rate * channels * bits_per_sample
    self->bitrate_kbps = (sample_rate * channels * bps) / 1000;

    if (data_bytes > 0) {
      self->duration_seconds =
          data_bytes / (sample_rate * channels * (bps / 8));
    }

    // Configure I2S dynamically for the WAV parameters
    i2s_channel_disable(self->tx_handle);
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    i2s_channel_reconfig_std_clock(self->tx_handle, &clk_cfg);
    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT,
        (channels == 2) ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO);
    i2s_channel_reconfig_std_slot(self->tx_handle, &slot_cfg);
    i2s_channel_enable(self->tx_handle);

    unsigned char *read_ptr = read_buf + data_offset;
    bytes_left -= data_offset;

    // Stream PCM directly to I2S
    while (!self->stop_request) {
      if (self->paused) {
        vTaskDelay(pdMS_TO_TICKS(50));
        continue;
      }

      // 1. PROACTIVE FEED: WAV files consume ~176KB/s. If we don't ask Python
      // for more data before we run out, the hardware will underrun.
      if (rb_available(&self->rb) < (RING_BUF_SIZE / 2) && !self->rb.eof) {
        audioplayer_request_feed(self);
      }

      int bytes_to_process = bytes_left & ~1; // Must be even for 16-bit
      if (bytes_to_process > 0) {
        apply_volume((int16_t *)read_ptr, bytes_to_process / 2,
                     self->volume_pct);
        size_t written = 0;
        i2s_channel_write(self->tx_handle, read_ptr, bytes_to_process, &written,
                          portMAX_DELAY);
        read_ptr += bytes_to_process;
        bytes_left -= bytes_to_process;
      }

      // 2. Slide remaining bytes to the front
      if (bytes_left > 0 && read_ptr != read_buf) {
        memmove(read_buf, read_ptr, bytes_left);
      }
      read_ptr = read_buf;

      if (self->rb.eof && rb_available(&self->rb) == 0 && bytes_left < 2) {
        break; // EOF reached cleanly
      }

      // 3. Refill read_buf from the ring buffer
      size_t space = AUDIO_READ_BUF_SIZE - bytes_left;
      if (space > 0) {
        size_t n = rb_read(&self->rb, read_buf + bytes_left, space);
        bytes_left += n;
      }

      // 4. Yield if starved: If the buffer runs completely dry, yield to the
      // scheduler so the Python thread actually gets CPU time to fulfill the
      // feed request.
      if (bytes_left < 4 && !self->rb.eof) {
        vTaskDelay(pdMS_TO_TICKS(10));
      }
    }

  } else {

    // ----- MP3 PLAYBACK LOOP -----
    ESP_LOGI(TAG, "Assuming MP3 format");

    // Skip a leading ID3v2 tag, if present, *before* scanning for MP3
    // sync words. This matters most on "real" files: ID3v2 tags often
    // carry an embedded cover image (an APIC frame), and JPEG's APPn
    // markers (byte pair 0xFF 0xE0-0xEF) collide directly with MP3's
    // 11-bit sync pattern. Left unskipped, MP3FindSyncWord() can lock
    // onto bytes inside the image, and every so often Helix reports a
    // "successful" decode for that bogus frame with samprate == 0 --
    // which then divides by zero inside the I2S driver's clock
    // calculation and crashes the whole chip (this is what happened:
    // bigger files have bigger embedded art, hence more chances of a
    // collision). Explicitly skipping the tag avoids scanning through
    // the image at all. See also the sanity check on info.samprate
    // below, which guards against any false sync that isn't inside an
    // ID3 tag.
    if (bytes_left >= 10 && memcmp(read_buf, "ID3", 3) == 0) {
      uint8_t version = read_buf[3];
      uint8_t flags = read_buf[5];
      uint32_t tag_size = ((read_buf[6] & 0x7F) << 21) |
                          ((read_buf[7] & 0x7F) << 14) |
                          ((read_buf[8] & 0x7F) << 7) | (read_buf[9] & 0x7F);
      uint32_t total = 10 + tag_size + ((flags & 0x10) ? 10 : 0);
      ESP_LOGI(TAG, "Parsing and skipping ID3v2 tag (%u bytes)",
               (unsigned)total);

      // --- NEW: Best-Effort ID3 Text Extraction ---
      // Text tags are usually in the first few kilobytes. We scan whatever we
      // have in read_buf before we start discarding it.
      int scan_len = (bytes_left < (int)total) ? bytes_left : (int)total;
      int pos = 10;

      while (pos + 10 <= scan_len) {
        if (read_buf[pos] == 0)
          break; // Hit padding, no more frames

        uint32_t fsize = (read_buf[pos + 4] << 24) | (read_buf[pos + 5] << 16) |
                         (read_buf[pos + 6] << 8) | read_buf[pos + 7];
        if (version == 4) { // ID3v2.4 uses syncsafe frame sizes
          fsize = ((read_buf[pos + 4] & 0x7F) << 21) |
                  ((read_buf[pos + 5] & 0x7F) << 14) |
                  ((read_buf[pos + 6] & 0x7F) << 7) |
                  (read_buf[pos + 7] & 0x7F);
        }

        if (pos + 10 + fsize > scan_len)
          break; // Frame runs past buffer

        if (memcmp(&read_buf[pos], "TIT2", 4) == 0) {
          extract_id3_string(&read_buf[pos + 10], fsize, self->id3_title,
                             sizeof(self->id3_title));
        } else if (memcmp(&read_buf[pos], "TPE1", 4) == 0) {
          extract_id3_string(&read_buf[pos + 10], fsize, self->id3_artist,
                             sizeof(self->id3_artist));
        } else if (memcmp(&read_buf[pos], "TALB", 4) == 0) {
          extract_id3_string(&read_buf[pos + 10], fsize, self->id3_album,
                             sizeof(self->id3_album));
        } else if (memcmp(&read_buf[pos], "TLEN", 4) == 0) {
          char tlen_str[16];
          extract_id3_string(&read_buf[pos + 10], fsize, tlen_str,
                             sizeof(tlen_str));
          self->duration_seconds = atoi(tlen_str) / 1000;
        }
        pos += 10 + fsize;
      }

      uint32_t skipped = 0;
      while (!self->stop_request && skipped < total) {
        uint32_t take = total - skipped;
        if (take > (uint32_t)bytes_left) {
          take = (uint32_t)bytes_left;
        }
        if (take > 0) {
          if (take < (uint32_t)bytes_left) {
            memmove(read_buf, read_buf + take, bytes_left - take);
          }
          bytes_left -= (int)take;
          skipped += take;
        }
        if (skipped >= total) {
          break;
        }
        if (self->rb.eof && rb_available(&self->rb) == 0) {
          // Tag claims to run past the end of the file; give up
          // skipping and let the normal sync-word scan handle
          // whatever's left (should be nothing, in practice).
          break;
        }
        size_t space = AUDIO_READ_BUF_SIZE - bytes_left;
        size_t n = rb_read(&self->rb, read_buf + bytes_left, space);
        bytes_left += (int)n;
        if (n == 0) {
          audioplayer_request_feed(self);
          vTaskDelay(pdMS_TO_TICKS(10));
        }
      }
    }

    HMP3Decoder decoder = MP3InitDecoder();
    if (decoder == NULL) {
      self->last_error = -MP_ENOMEM;
      goto done;
    }
    int16_t *pcm_buf = heap_caps_malloc(
        AUDIO_MAX_FRAME_SAMPS * 2 * sizeof(int16_t), MALLOC_CAP_8BIT);
    if (pcm_buf == NULL) {
      self->last_error = -MP_ENOMEM;
      MP3FreeDecoder(decoder);
      goto done;
    }

    unsigned char *read_ptr = read_buf;
    uint32_t loop_iters = 0;
    uint32_t err_streak = 0;

    while (!self->stop_request) {
      if (self->paused) {
        vTaskDelay(pdMS_TO_TICKS(50));
        continue;
      }
      if ((++loop_iters & 0x3F) == 0) {
        vTaskDelay(1);
      }

      if (bytes_left < AUDIO_REFILL_THRESHOLD) {
        if (read_ptr != read_buf && bytes_left > 0) {
          memmove(read_buf, read_ptr, bytes_left);
        }
        read_ptr = read_buf;
        size_t space = AUDIO_READ_BUF_SIZE - bytes_left;
        size_t n = rb_read(&self->rb, read_buf + bytes_left, space);
        bytes_left += (int)n;

        if (bytes_left < AUDIO_REFILL_THRESHOLD) {
          if (!self->rb.eof) {
            audioplayer_request_feed(self);
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
          }
        }
      }

      if (bytes_left <= 0)
        break;

      int offset = MP3FindSyncWord(read_ptr, bytes_left);
      if (offset < 0) {
        bytes_left = 0;
        if (self->rb.eof)
          break;
        continue;
      }
      read_ptr += offset;
      bytes_left -= offset;

      int err = MP3Decode(decoder, &read_ptr, &bytes_left, pcm_buf, 0);
      if (err != 0) {
        if (self->rb.eof && bytes_left < 2)
          break;
        if ((++err_streak & 0xFF) == 0) {
          ESP_LOGW(TAG, "MP3Decode err %d", err);
        }
        if (bytes_left > 0) {
          read_ptr += 1;
          bytes_left -= 1;
        }
        continue;
      }

      MP3FrameInfo info;
      MP3GetLastFrameInfo(decoder, &info);

      // Belt-and-braces on top of the ID3v2 skip above: never trust a
      // decoded frame's info enough to hand it to the I2S driver without
      // checking it first. A false sync match elsewhere in the stream
      // (not just inside a tag) can still make Helix report success
      // with an implausible samprate/nChans -- and samprate == 0 in
      // particular divides by zero inside i2s_channel_reconfig_std_clock()
      // and takes the whole chip down, not just this task.
      if (info.samprate <= 0 || info.samprate > 192000 || info.nChans < 1 ||
          info.nChans > 2 || info.outputSamps <= 0 ||
          info.outputSamps > AUDIO_MAX_FRAME_SAMPS * 2) {
        if ((++err_streak & 0xFF) == 0) {
          ESP_LOGW(TAG,
                   "implausible frame info (samprate=%d chans=%d samps=%d) "
                   "-- treating as a bad sync and resyncing",
                   info.samprate, info.nChans, info.outputSamps);
        }
        continue;
      }
      err_streak = 0;

      if (info.bitrate > 0) {
        self->bitrate_kbps = info.bitrate / 1000;
      }

      if (info.samprate != self->last_samprate ||
          info.nChans != self->last_channels) {
        i2s_channel_disable(self->tx_handle);
        i2s_std_clk_config_t clk_cfg =
            I2S_STD_CLK_DEFAULT_CONFIG(info.samprate);
        i2s_channel_reconfig_std_clock(self->tx_handle, &clk_cfg);
        i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            (info.nChans == 2) ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO);
        i2s_channel_reconfig_std_slot(self->tx_handle, &slot_cfg);
        i2s_channel_enable(self->tx_handle);

        self->last_samprate = info.samprate;
        self->last_channels = info.nChans;
      }

      int n_samples = info.outputSamps;
      apply_volume(pcm_buf, n_samples, self->volume_pct);
      size_t bytes_written = 0;
      i2s_channel_write(self->tx_handle, pcm_buf, n_samples * sizeof(int16_t),
                        &bytes_written, portMAX_DELAY);
    }
    free(pcm_buf);
    MP3FreeDecoder(decoder);
  }

done:
  // Safe DMA zero-flush
  {
    size_t bw = 0;
    int16_t *zero_buf = calloc(AUDIO_MAX_FRAME_SAMPS * 2, sizeof(int16_t));
    if (zero_buf) {
      for (int i = 0; i < 2; i++) {
        i2s_channel_write(self->tx_handle, zero_buf,
                          AUDIO_MAX_FRAME_SAMPS * 2 * sizeof(int16_t), &bw,
                          portMAX_DELAY);
      }
      free(zero_buf);
    }
  }
  if (read_buf)
    free(read_buf);

done_no_buf:
  ESP_LOGI(TAG, "playback task exiting");
  self->playing = false;
  xSemaphoreGive(self->done_sem);
  vTaskDelete(NULL);
}
// ---- Python-visible methods --------------------------------------------

static mp_obj_t audioplayer_make_new(const mp_obj_type_t *type, size_t n_args,
                                     size_t n_kw, const mp_obj_t *args) {
  enum {
    ARG_bck,
    ARG_ws,
    ARG_dout,
    ARG_i2s_num,
    ARG_dma_buf_count,
    ARG_dma_buf_len
  };
  static const mp_arg_t allowed_args[] = {
      {MP_QSTR_bck, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
      {MP_QSTR_ws, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
      {MP_QSTR_dout, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
      {MP_QSTR_i2s_num, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0}},
      {MP_QSTR_dma_buf_count, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 6}},
      {MP_QSTR_dma_buf_len, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 512}},
  };

  mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)];
  mp_arg_parse_all_kw_array(n_args, n_kw, args, MP_ARRAY_SIZE(allowed_args),
                            allowed_args, parsed);

  audioplayer_obj_t *self = mp_obj_malloc(audioplayer_obj_t, type);
  self->port_num = (i2s_port_t)parsed[ARG_i2s_num].u_int;
  self->tx_handle = NULL;
  self->task = NULL;
  self->stack = NULL;
  self->playing = false;
  self->stop_request = false;
  self->paused = false;
  self->last_error = 0;
  self->volume_pct = 70;
  self->file_obj = mp_const_none;
  self->feed_pending = false;
  self->i2s_installed = false;
  self->done_sem = xSemaphoreCreateBinary();
  if (!rb_init(&self->rb, RING_BUF_SIZE)) {
    mp_raise_OSError(MP_ENOMEM);
  }

  // 1. Allocate the I2S channel
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(self->port_num, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;
  chan_cfg.dma_desc_num = parsed[ARG_dma_buf_count].u_int;
  chan_cfg.dma_frame_num = parsed[ARG_dma_buf_len].u_int;

  esp_err_t ret = i2s_new_channel(&chan_cfg, &self->tx_handle, NULL);
  if (ret != ESP_OK) {
    mp_raise_OSError(MP_EIO);
  }

  // 2. Configure it for standard mode (Philips I2S)
  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,
              .bclk = parsed[ARG_bck].u_int,
              .ws = parsed[ARG_ws].u_int,
              .dout = parsed[ARG_dout].u_int,
              .din = I2S_GPIO_UNUSED,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };

  ret = i2s_channel_init_std_mode(self->tx_handle, &std_cfg);
  if (ret != ESP_OK) {
    i2s_del_channel(self->tx_handle);
    mp_raise_OSError(MP_EIO);
  }

  i2s_channel_enable(self->tx_handle);
  self->i2s_installed = true;

  return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audioplayer_play(mp_obj_t self_in, mp_obj_t filename_in) {
  audioplayer_obj_t *self = MP_OBJ_TO_PTR(self_in);

  if (self->playing) {
    mp_raise_msg(&mp_type_RuntimeError,
                 MP_ERROR_TEXT("already playing; call stop() first"));
  }

  // Reclaim the previous track's PSRAM stack if play() is being called
  // again without an intervening stop() (e.g. advancing a playlist after
  // natural EOF). self->playing == false (just confirmed above) means
  // audio_play_task() already reached its own "self->playing = false;
  // xSemaphoreGive(done_sem)" exit sequence -- take done_sem (returns
  // immediately, since it's already given) to synchronize with that
  // before freeing the stack out from under it, same reasoning as
  // audioplayer_stop()'s identical take just below.
  if (self->stack != NULL) {
    xSemaphoreTake(self->done_sem, pdMS_TO_TICKS(2000));
    heap_caps_free(self->stack);
    self->stack = NULL;
  }

  mp_obj_t file;
  if (mp_obj_is_str(filename_in)) {
    // Open via MicroPython's own open(), not fopen() -- this is what
    // makes the module VFS-aware: it works for anything mounted through
    // MicroPython's VFS (internal flash, SD cards mounted with
    // os.mount(), etc.), not just paths ESP-IDF's own POSIX layer
    // happens to see. Lets a bad path raise a normal Python
    // OSError/FileNotFoundError.
    mp_obj_t open_fn = mp_load_global(MP_QSTR_open);
    mp_obj_t open_args[2] = {filename_in, MP_OBJ_NEW_QSTR(MP_QSTR_rb)};
    file = mp_call_function_n_kw(open_fn, 2, 0, open_args);
  } else {
    // Already a stream-like object (e.g. a live socket handed in for
    // radio streaming -- an HTTP response body with no Content-Length
    // is just the raw, still-open socket). Caller owns
    // opening/connecting it; validate the stream protocol up front, on
    // the MP thread, so a bad object raises a clear error here instead
    // of deep inside the first feed.
    mp_get_stream_raise(filename_in, MP_STREAM_OP_READ);
    file = filename_in;
  }

  self->file_obj = file;
  self->last_error = 0;
  self->id3_title[0] = '\0';
  self->id3_artist[0] = '\0';
  self->id3_album[0] = '\0';
  self->bitrate_kbps = 0;
  self->duration_seconds = 0;
  self->paused = false;
  rb_reset(&self->rb);

  // Prime the buffer synchronously (we're already on the MP thread here)
  // so the decode task has data to chew on the instant it starts.
  for (int i = 0; i < FEED_PRIME_ROUNDS; i++) {
    if (audioplayer_feed_internal(self) <= 0) {
      break;
    }
  }

  self->stop_request = false;
  self->playing = true;

  // PSRAM-backed stack -- see modules/modssh/modssh.c's ssh_client_connect()
  // for the full rationale (same pattern).
  StackType_t *stack = heap_caps_malloc(AUDIO_TASK_STACK_WORDS * sizeof(StackType_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (stack == NULL) {
    stack = heap_caps_malloc(AUDIO_TASK_STACK_WORDS * sizeof(StackType_t),
                             MALLOC_CAP_8BIT);
  }
  if (stack == NULL) {
    self->playing = false;
    mp_raise_msg(&mp_type_RuntimeError,
                 MP_ERROR_TEXT("failed to allocate playback task stack"));
  }
  self->stack = stack;

  TaskHandle_t task = xTaskCreateStaticPinnedToCore(
      audio_play_task, "audioplayer", AUDIO_TASK_STACK_WORDS, self,
      AUDIO_TASK_PRIORITY, stack, &self->task_buf,
      1 /* APP CPU -- leave PRO CPU/core 0 for MicroPython */);

  if (task == NULL) {
    heap_caps_free(stack);
    self->stack = NULL;
    self->playing = false;
    mp_raise_msg(&mp_type_RuntimeError,
                 MP_ERROR_TEXT("failed to start playback task"));
  }
  self->task = task;

  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audioplayer_play_obj, audioplayer_play);

// Manual pump, for callers who'd rather not rely on mp_sched_schedule
// (e.g. tight native loops that never hit a scheduler safe-point).
// Returns True if data was queued or is already buffered, False at EOF.
static mp_obj_t audioplayer_feed(mp_obj_t self_in) {
  audioplayer_obj_t *self = MP_OBJ_TO_PTR(self_in);
  int r = audioplayer_feed_internal(self);
  return mp_obj_new_bool(r > 0 || rb_available(&self->rb) > 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioplayer_feed_obj, audioplayer_feed);

// duration(path, exact=False) -> float seconds. Doesn't require play()
// to have been called first, and doesn't touch playback state at all --
// this opens its own independent file handle. See the "Duration
// probing" section above for the technique breakdown.
static mp_obj_t audioplayer_duration(size_t n_args, const mp_obj_t *pos_args,
                                     mp_map_t *kw_args) {
  enum { ARG_exact };
  static const mp_arg_t allowed_args[] = {
      {MP_QSTR_exact, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false}},
  };
  mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)];
  mp_arg_parse_all(n_args - 2, pos_args + 2, kw_args,
                   MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);
  bool want_exact = parsed[ARG_exact].u_bool;
  mp_obj_t path_obj = pos_args[1];

  mp_obj_t open_fn = mp_load_global(MP_QSTR_open);
  mp_obj_t open_call_args[2] = {path_obj, MP_OBJ_NEW_QSTR(MP_QSTR_rb)};
  mp_obj_t file = mp_call_function_n_kw(open_fn, 2, 0, open_call_args);

  mp_int_t file_size = duration_stream_seek(file, 0, 2 /* SEEK_END */);
  duration_stream_seek(file, 0, 0 /* SEEK_SET */);

  const mp_stream_p_t *stream_p = mp_get_stream_raise(file, MP_STREAM_OP_READ);

  uint8_t *buf = heap_caps_malloc(DURATION_PROBE_BUF, MALLOC_CAP_8BIT);
  if (buf == NULL) {
    mp_obj_t close_meth[2];
    mp_load_method(file, MP_QSTR_close, close_meth);
    mp_call_method_n_kw(0, 0, close_meth);
    mp_raise_OSError(MP_ENOMEM);
  }

  int errcode = 0;
  size_t bytes_left = stream_p->read(file, buf, DURATION_PROBE_BUF, &errcode);
  if (bytes_left == MP_STREAM_ERROR) {
    bytes_left = 0;
  }

  // Skip a leading ID3v2 tag, same parsing as playback -- see the note
  // in audio_play_task() about why this matters (embedded album art
  // confuses naive sync-word scanning).
  size_t id3_total = 0;
  if (bytes_left >= 10 && memcmp(buf, "ID3", 3) == 0) {
    uint8_t flags = buf[5];
    uint32_t tag_size = ((buf[6] & 0x7F) << 21) | ((buf[7] & 0x7F) << 14) |
                        ((buf[8] & 0x7F) << 7) | (buf[9] & 0x7F);
    id3_total = 10 + tag_size + ((flags & 0x10) ? 10 : 0);

    while (bytes_left < id3_total && bytes_left < DURATION_PROBE_BUF) {
      size_t before = bytes_left;
      bytes_left =
          duration_refill(file, stream_p, buf, bytes_left, DURATION_PROBE_BUF);
      if (bytes_left == before) {
        break;
      }
    }
    if (id3_total < bytes_left) {
      memmove(buf, buf + id3_total, bytes_left - id3_total);
      bytes_left -= id3_total;
    } else {
      bytes_left = 0; // tag ate everything we've buffered (and maybe more)
    }
  }

  double duration_sec = -1.0;

  if (!want_exact && bytes_left >= 4) {
    int offset = MP3FindSyncWord(buf, bytes_left);
    if (offset >= 0 && (size_t)offset + 4 <= bytes_left) {
      HMP3Decoder probe_dec = MP3InitDecoder();
      MP3FrameInfo info;
      int err = MP3GetNextFrameInfo(probe_dec, &info, buf + offset);
      MP3FreeDecoder(probe_dec);

      if (err == 0 && info.samprate > 0) {
        int samples_per_frame = (info.version == 0) ? 1152 : 576;
        uint32_t num_frames = 0;
        if (find_vbr_header(buf + offset, bytes_left - offset, &info,
                            &num_frames) == 0 &&
            num_frames > 0) {
          duration_sec = (double)num_frames * samples_per_frame / info.samprate;
        } else if (info.bitrate > 0) {
          mp_int_t audio_bytes = file_size - (mp_int_t)id3_total - offset;
          if (audio_bytes > 0) {
            duration_sec = (double)audio_bytes * 8.0 / info.bitrate;
          }
        }
      }
    }
  }

  if (duration_sec < 0) {
    // exact=True, or the fast path had nothing to work with -- walk
    // every frame header in the file instead.
    duration_sec =
        mp3_duration_full_scan(file, buf, DURATION_PROBE_BUF, bytes_left);
  }

  free(buf);
  mp_obj_t close_meth[2];
  mp_load_method(file, MP_QSTR_close, close_meth);
  mp_call_method_n_kw(0, 0, close_meth);

  if (duration_sec < 0) {
    mp_raise_ValueError(MP_ERROR_TEXT("could not determine MP3 duration"));
  }

  return mp_obj_new_float((mp_float_t)duration_sec);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audioplayer_duration_obj, 2,
                                  audioplayer_duration);

static void audioplayer_close_file(audioplayer_obj_t *self) {
  if (self->file_obj != mp_const_none) {
    mp_obj_t close_meth[2];
    mp_load_method(self->file_obj, MP_QSTR_close, close_meth);
    mp_call_method_n_kw(0, 0, close_meth);
    self->file_obj = mp_const_none;
  }
}

static mp_obj_t audioplayer_pause(mp_obj_t self_in) {
  audioplayer_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (self->playing) {
    self->paused = true;
  }
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioplayer_pause_obj, audioplayer_pause);

static mp_obj_t audioplayer_resume(mp_obj_t self_in) {
  audioplayer_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (self->playing) {
    self->paused = false;
  }
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioplayer_resume_obj, audioplayer_resume);

static mp_obj_t audioplayer_is_paused(mp_obj_t self_in) {
  audioplayer_obj_t *self = MP_OBJ_TO_PTR(self_in);
  return mp_obj_new_bool(self->paused);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioplayer_is_paused_obj,
                                 audioplayer_is_paused);

static mp_obj_t audioplayer_stop(mp_obj_t self_in) {
  audioplayer_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (self->playing) {
    self->stop_request = true;
    // Only free the PSRAM stack if the task actually confirmed exit --
    // on a timeout it may still be running on it.
    if (xSemaphoreTake(self->done_sem, pdMS_TO_TICKS(2000)) == pdTRUE &&
        self->stack != NULL) {
      heap_caps_free(self->stack);
      self->stack = NULL;
    }
  }
  audioplayer_close_file(self);
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioplayer_stop_obj, audioplayer_stop);

static mp_obj_t audioplayer_is_playing(mp_obj_t self_in) {
  audioplayer_obj_t *self = MP_OBJ_TO_PTR(self_in);
  return mp_obj_new_bool(self->playing);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioplayer_is_playing_obj,
                                 audioplayer_is_playing);

static mp_obj_t audioplayer_last_error(mp_obj_t self_in) {
  audioplayer_obj_t *self = MP_OBJ_TO_PTR(self_in);
  return mp_obj_new_int(self->last_error);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioplayer_last_error_obj,
                                 audioplayer_last_error);

static mp_obj_t audioplayer_volume(size_t n_args, const mp_obj_t *args) {
  audioplayer_obj_t *self = MP_OBJ_TO_PTR(args[0]);
  if (n_args == 1) {
    return mp_obj_new_int(self->volume_pct);
  }
  mp_int_t v = mp_obj_get_int(args[1]);
  if (v < 0)
    v = 0;
  if (v > 100)
    v = 100;
  self->volume_pct = (int)v;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audioplayer_volume_obj, 1, 2,
                                           audioplayer_volume);

static mp_obj_t audioplayer_deinit(mp_obj_t self_in) {
  audioplayer_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (self->playing) {
    self->stop_request = true;
    // Only free the PSRAM stack if the task actually confirmed exit --
    // on a timeout it may still be running on it.
    if (xSemaphoreTake(self->done_sem, pdMS_TO_TICKS(2000)) == pdTRUE &&
        self->stack != NULL) {
      heap_caps_free(self->stack);
      self->stack = NULL;
    }
  }
  audioplayer_close_file(self);
  rb_deinit(&self->rb);
  if (self->i2s_installed) {
    i2s_channel_disable(self->tx_handle);
    i2s_del_channel(self->tx_handle);
    self->i2s_installed = false;
  }
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioplayer_deinit_obj, audioplayer_deinit);

static mp_obj_t audioplayer_tags(mp_obj_t self_in) {
  audioplayer_obj_t *self = MP_OBJ_TO_PTR(self_in);
  mp_obj_t dict = mp_obj_new_dict(0);

  mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_title),
                    mp_obj_new_str(self->id3_title, strlen(self->id3_title)));
  mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_artist),
                    mp_obj_new_str(self->id3_artist, strlen(self->id3_artist)));
  mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_album),
                    mp_obj_new_str(self->id3_album, strlen(self->id3_album)));
  mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_bitrate),
                    mp_obj_new_int(self->bitrate_kbps));
  mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_duration),
                    mp_obj_new_int(self->duration_seconds));

  return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioplayer_tags_obj, audioplayer_tags);

static const mp_rom_map_elem_t audioplayer_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audioplayer_play_obj)},
    {MP_ROM_QSTR(MP_QSTR_pause), MP_ROM_PTR(&audioplayer_pause_obj)},
    {MP_ROM_QSTR(MP_QSTR_duration), MP_ROM_PTR(&audioplayer_duration_obj)},
    {MP_ROM_QSTR(MP_QSTR_resume), MP_ROM_PTR(&audioplayer_resume_obj)},
    {MP_ROM_QSTR(MP_QSTR_is_paused), MP_ROM_PTR(&audioplayer_is_paused_obj)},
    {MP_ROM_QSTR(MP_QSTR_feed), MP_ROM_PTR(&audioplayer_feed_obj)},
    {MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&audioplayer_stop_obj)},
    {MP_ROM_QSTR(MP_QSTR_is_playing), MP_ROM_PTR(&audioplayer_is_playing_obj)},
    {MP_ROM_QSTR(MP_QSTR_volume), MP_ROM_PTR(&audioplayer_volume_obj)},
    {MP_ROM_QSTR(MP_QSTR_last_error), MP_ROM_PTR(&audioplayer_last_error_obj)},
    {MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audioplayer_deinit_obj)},
    {MP_ROM_QSTR(MP_QSTR_tags), MP_ROM_PTR(&audioplayer_tags_obj)},
};
static MP_DEFINE_CONST_DICT(audioplayer_locals_dict,
                            audioplayer_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(audioplayer_AudioPlayer_type, MP_QSTR_AudioPlayer,
                         MP_TYPE_FLAG_NONE, make_new, audioplayer_make_new,
                         locals_dict, &audioplayer_locals_dict);

// ---- module table -------------------------------------------------------

static const mp_rom_map_elem_t audioplayer_module_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audioplayer)},
    {MP_ROM_QSTR(MP_QSTR_AudioPlayer),
     MP_ROM_PTR(&audioplayer_AudioPlayer_type)},
    {MP_ROM_QSTR(MP_QSTR_AudioRecorder),
     MP_ROM_PTR(&audiorecorder_AudioRecorder_type)},
};
static MP_DEFINE_CONST_DICT(audioplayer_module_globals,
                            audioplayer_module_globals_table);

const mp_obj_module_t audioplayer_user_cmodule = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&audioplayer_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audioplayer, audioplayer_user_cmodule);
