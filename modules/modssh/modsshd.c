/*
 * MicroPython SSH server (wolfSSH-backed)
 * Copyright (c) 2026 8bitmcu
 * License: MIT
 *
 * Unlike a typical SSH server, this does NOT spawn an isolated remote
 * shell per connection -- it shares whatever shell is already running on
 * the physical device. Incoming bytes are fed into env.kvm's existing
 * "ghost key" injection mechanism (kvm.inject()/internal_inject_n(),
 * tdeck_kvm.c -- the same mechanism shell.py's Ctrl-A raw-REPL handoff
 * uses), indistinguishable from a real physical keypress. Outgoing bytes
 * are mirrored out via kvm.set_mirror() -- this port's MicroPython build
 * only has one os.dupterm() slot, already occupied by env.kvm itself.
 *
 * Single session only: the listener does not accept a second connection
 * while one is already active.
 *
 * Registered as its own "modsshd" module (Server type) rather than
 * folded into modssh.Client -- mirrors the ssh.py/sshd.py application
 * split, and avoids touching modssh.c. Still built into the same
 * usermod_modssh CMake target so it picks up the wolfSSH vendor patches
 * established there automatically.
 *
 * Architecture: same dedicated-FreeRTOS-task-pinned-to-core-1 pattern as
 * modssh.c's SSH client, and the same select()-gated steady-state loop
 * (see modssh.c's ssh_task() for why SO_RCVTIMEO breaks
 * wolfSSH_stream_read() and must never be reintroduced here).
 *
 * Cross-task safety: the background task never touches MicroPython's
 * own heap/GC/VM state directly (see modssh.c's header comment). Output
 * crosses the task boundary through a ring_buf_t: kvm's mirror write()
 * (mp_task) queues bytes there; this task drains it and calls
 * wolfSSH_stream_send(). The mirror is attached once in start()
 * (mp_task) and stays attached for the Server's whole lifetime.
 *
 * The connect/disconnect notifier (set_notify()) is the one place this
 * task reaches toward Python, via mp_sched_schedule() rather than a
 * direct call -- see set_notify()'s comment.
 *
 * The one exception: incoming data is fed to internal_inject_n()
 * directly from this background task. That function's ring buffer
 * (tdeck_kvm.c's static inject_buf) is NOT mutex-protected -- it was
 * only ever single-producer before this. A second, cross-core producer
 * here is a real, narrow gap: a rare simultaneous local-keypress-plus-
 * incoming-ssh-byte race could drop one byte. Can't corrupt memory or
 * crash (bounds check still applies); low-consequence enough that a
 * full ring_buf_t relay isn't built for this first pass.
 */

#include "py/obj.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "py/mphal.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h" // xTaskCreatePinnedToCore, see modssh.c
#include "esp_heap_caps.h" // heap_caps_malloc(MALLOC_CAP_SPIRAM), see modules/modc2/modc2_alloc.c

#include "ring_buf.h"

#include <wolfssh/ssh.h>
#include <wolfssh/error.h>
#include <wolfssh/keygen.h>

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>

// Defined (non-static) in tdeck_kvm.c -- same cross-module extern
// pattern that file already uses for vt_VT_type/tdeck_kbd_type. All
// usermod targets link into one firmware image, so no extra CMake
// wiring is needed for this to resolve.
extern void internal_inject_n(const char *data, size_t len);

#define SSHD_TASK_STACK_WORDS 16384
#define SSHD_TASK_PRIORITY 5
#define SSHD_RB_SIZE 4096
#define SSHD_RECV_TIMEOUT_MS 100
#define SSHD_ACCEPT_POLL_MS 200
#define SSHD_HOST_KEY_MAX_SZ 512

// How long to wait for a freshly accept()ed client's first byte before
// giving up -- see sshd_handle_client()'s comment.
#define SSHD_HANDSHAKE_TIMEOUT_MS 10000

// How long a connected session may go without the *client* sending
// anything before it's dropped -- only resets on incoming data, never
// outgoing (SO_KEEPALIVE below is a complementary earlier detector).
#define SSHD_IDLE_TIMEOUT_MS (30 * 60 * 1000)

typedef enum {
  SSHD_STATE_IDLE = 0,
  SSHD_STATE_LISTENING,
  SSHD_STATE_CONNECTED,
  SSHD_STATE_STOPPED,
} sshd_state_t;

typedef struct _sshd_server_obj_t {
  mp_obj_base_t base;

  TaskHandle_t task;
  StaticTask_t task_buf; // xTaskCreateStaticPinnedToCore()'s control block
  StackType_t *stack;    // PSRAM-backed, see sshd_server_start()
  volatile sshd_state_t state;
  volatile bool stop_request;

  mp_obj_t kvm; // env.kvm -- only ever touched from mp_task (start()/stop())

  // Optional connect/disconnect notifier -- see set_notify().
  mp_obj_t notify_cb;

  char username[32];
  char password[64];

  byte host_key[SSHD_HOST_KEY_MAX_SZ];
  word32 host_key_sz;

  int port;
  int listen_fd;
  int client_fd;
  char client_ip[16]; // dotted-quad, set right after accept() in sshd_task()

  ring_buf_t tx; // kvm mirror write() (mp_task) -> this task (consumer)

  // Diagnostic only -- see modssh.c's identical last_error field.
  // Sentinels chosen the same way: well clear of wolfssh/error.h's
  // whole WS_* span (continuous from -1 to at least -1097).
  int last_error;
} sshd_server_obj_t;

#define SSHD_ERR_SOCKET -4000
#define SSHD_ERR_BIND -4001
#define SSHD_ERR_LISTEN -4002
#define SSHD_ERR_CTX_NEW -4003
#define SSHD_ERR_KEY -4004
#define SSHD_ERR_SSH_NEW -4005
#define SSHD_ERR_HANDSHAKE_TIMEOUT -4006
#define SSHD_ERR_IDLE_TIMEOUT -4007

const mp_obj_type_t sshd_server_type;

// wolfSSH_SetUserAuth callback -- server side. Unlike modssh.c's
// ssh_user_auth() (which SUPPLIES credentials), this VERIFIES the
// client-supplied username/password. Password auth only -- no
// keyboard-interactive server-side support.
static int sshd_user_auth(byte authType, WS_UserAuthData *authData, void *ctx) {
  sshd_server_obj_t *self = (sshd_server_obj_t *)ctx;

  if (authType != WOLFSSH_USERAUTH_PASSWORD) {
    return WOLFSSH_USERAUTH_FAILURE;
  }

  size_t userLen = strlen(self->username);
  if (authData->usernameSz != userLen ||
      memcmp(authData->username, self->username, userLen) != 0) {
    return WOLFSSH_USERAUTH_INVALID_USER;
  }

  size_t passLen = strlen(self->password);
  if (authData->sf.password.passwordSz != passLen ||
      memcmp(authData->sf.password.password, self->password, passLen) != 0) {
    return WOLFSSH_USERAUTH_INVALID_PASSWORD;
  }

  return WOLFSSH_USERAUTH_SUCCESS;
}

// Handles one already-accept()ed connection start to finish: auth,
// channel/pty/shell setup (all handled internally by wolfSSH_accept(),
// the server-side counterpart of modssh.c's wolfSSH_connect()), then
// the steady-state loop. Blocks for the whole session -- sshd_task()'s
// caller loop only accept()s again once this returns, which is what
// makes this single-session by construction.
static void sshd_handle_client(sshd_server_obj_t *self) {
  WOLFSSH_CTX *ctx = NULL;
  WOLFSSH *ssh = NULL;
  // Only fire "disconnected" if we actually notified "connected" -- a
  // client that never gets past the handshake shouldn't produce a
  // disconnect toast for a connection nobody was told about.
  bool notified_connect = false;

  // Discard any mirror output queued while nobody was connected, so the
  // new session starts clean instead of getting a burst of stale output.
  rb_reset(&self->tx);

  ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, NULL);
  if (ctx == NULL) {
    self->last_error = SSHD_ERR_CTX_NEW;
    goto done;
  }

  if (wolfSSH_CTX_UsePrivateKey_buffer(ctx, self->host_key, self->host_key_sz,
                                       WOLFSSH_FORMAT_ASN1) != WS_SUCCESS) {
    self->last_error = SSHD_ERR_KEY;
    goto done;
  }

  wolfSSH_SetUserAuth(ctx, sshd_user_auth);

  ssh = wolfSSH_new(ctx);
  if (ssh == NULL) {
    self->last_error = SSHD_ERR_SSH_NEW;
    goto done;
  }

  wolfSSH_SetUserAuthCtx(ssh, (void *)self);
  wolfSSH_set_fd(ssh, self->client_fd);

  // Bound how long we wait for the client's first byte -- without this,
  // a TCP connection that never speaks hangs wolfSSH_accept() forever,
  // permanently stranding the single-session slot. Must be a pre-call
  // select() gate, not a non-blocking retry loop: wolfSSH_accept()'s
  // ACCEPT_* state machine treats any internal WS_WANT_READ as
  // immediately fatal with no retry (checked with a bare `< WS_SUCCESS`
  // in src/ssh.c), so a non-blocking socket would reintroduce the same
  // EAGAIN-reaches-wolfSSH bug this file's header comment warns about.
  // This only catches a peer that never sends anything at all -- one
  // that stalls mid-handshake can still block this call.
  {
    fd_set hfds;
    FD_ZERO(&hfds);
    FD_SET(self->client_fd, &hfds);
    struct timeval htv;
    htv.tv_sec = SSHD_HANDSHAKE_TIMEOUT_MS / 1000;
    htv.tv_usec = (SSHD_HANDSHAKE_TIMEOUT_MS % 1000) * 1000;
    if (select(self->client_fd + 1, &hfds, NULL, NULL, &htv) <= 0) {
      self->last_error = SSHD_ERR_HANDSHAKE_TIMEOUT;
      goto done;
    }
  }

  if (wolfSSH_accept(ssh) != WS_SUCCESS) {
    self->last_error = wolfSSH_get_error(ssh);
    goto done;
  }

  self->state = SSHD_STATE_CONNECTED;

  if (self->notify_cb != mp_const_none) {
    mp_sched_schedule(self->notify_cb, mp_const_true);
    notified_connect = true;
  }

  // Nudge the shared shell to redraw its prompt so the new SSH session
  // shows something immediately instead of a blank terminal -- the
  // banner/prompt already on screen predate this connection. shell.py's
  // _read_line() treats a bare '\r' as Enter on an empty line, which
  // just reprints the prompt (empty commands are a no-op). Also redraws
  // on the physical screen -- a fitting side effect of a shared shell.
  internal_inject_n("\r", 1);

  // Steady state: select()-gated, same as modssh.c's ssh_task() and for
  // the same SO_RCVTIMEO reason.
  //
  // last_activity tracks time since the last byte actually received
  // FROM the client (never reset by outgoing shell output). Without a
  // network-level FIN/RST, nothing else here would notice a vanished
  // peer, permanently stranding the single-session slot.
  uint8_t iobuf[256];
  TickType_t last_activity = xTaskGetTickCount();
  while (!self->stop_request) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(self->client_fd, &rfds);
    struct timeval tv;
    tv.tv_sec = SSHD_RECV_TIMEOUT_MS / 1000;
    tv.tv_usec = (SSHD_RECV_TIMEOUT_MS % 1000) * 1000;
    int sel = select(self->client_fd + 1, &rfds, NULL, NULL, &tv);

    if (sel > 0 && FD_ISSET(self->client_fd, &rfds)) {
      int n = wolfSSH_stream_read(ssh, iobuf, sizeof(iobuf));
      if (n > 0) {
        last_activity = xTaskGetTickCount();

        // Ctrl-D (0x04, EOF) ends only this SSH session, before
        // anything reaches the shared shell -- shell.py has no way to
        // tell an injected remote keystroke from a real physical one,
        // so this can't be handled shell-side without risking the
        // physical session too. Bytes preceding a Ctrl-D in the same
        // chunk are still delivered; anything after is discarded.
        const uint8_t *eof = memchr(iobuf, 0x04, (size_t)n);
        if (eof) {
          size_t before = (size_t)(eof - iobuf);
          if (before > 0) {
            internal_inject_n((const char *)iobuf, before);
          }
          self->last_error = 0; // clean, client-requested disconnect
          break;
        }

        internal_inject_n((const char *)iobuf, (size_t)n);
      } else if (n != WS_WANT_READ) {
        // WS_EOF / WS_CHANNEL_CLOSED / WS_DISCONNECT / any other error --
        // all mean this session is over.
        self->last_error = wolfSSH_get_error(ssh);
        break;
      }
    }

    if ((xTaskGetTickCount() - last_activity) * portTICK_PERIOD_MS >=
        SSHD_IDLE_TIMEOUT_MS) {
      self->last_error = SSHD_ERR_IDLE_TIMEOUT;
      break;
    }

    size_t avail = rb_available(&self->tx);
    if (avail > 0) {
      size_t chunk = avail < sizeof(iobuf) ? avail : sizeof(iobuf);
      size_t got = rb_read(&self->tx, iobuf, chunk);
      if (got > 0) {
        wolfSSH_stream_send(ssh, iobuf, (word32)got);
      }
    }
  }

done:
  if (notified_connect && self->notify_cb != mp_const_none) {
    mp_sched_schedule(self->notify_cb, mp_const_false);
  }
  if (ssh) {
    wolfSSH_shutdown(ssh);
    wolfSSH_free(ssh);
  }
  if (ctx) {
    wolfSSH_CTX_free(ctx);
  }
}

static void sshd_task(void *arg) {
  sshd_server_obj_t *self = (sshd_server_obj_t *)arg;

  static bool wolfssh_lib_initialized = false;
  if (!wolfssh_lib_initialized) {
    wolfSSH_Init();
    wolfssh_lib_initialized = true;
  }

  self->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (self->listen_fd < 0) {
    self->last_error = SSHD_ERR_SOCKET;
    goto done;
  }

  {
    int opt = 1;
    setsockopt(self->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  }

  {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)self->port);

    if (bind(self->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
      self->last_error = SSHD_ERR_BIND;
      goto done;
    }
  }

  if (listen(self->listen_fd, 1) != 0) {
    self->last_error = SSHD_ERR_LISTEN;
    goto done;
  }

  self->state = SSHD_STATE_LISTENING;

  while (!self->stop_request) {
    // Poll accept() with a short timeout so stop_request gets noticed
    // promptly instead of blocking in accept() indefinitely.
    fd_set afds;
    FD_ZERO(&afds);
    FD_SET(self->listen_fd, &afds);
    struct timeval atv;
    atv.tv_sec = 0;
    atv.tv_usec = SSHD_ACCEPT_POLL_MS * 1000;
    int asel = select(self->listen_fd + 1, &afds, NULL, NULL, &atv);
    if (asel <= 0 || !FD_ISSET(self->listen_fd, &afds)) {
      continue;
    }

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int fd = accept(self->listen_fd, (struct sockaddr *)&client_addr,
                    &client_len);
    if (fd < 0) {
      continue;
    }
    self->client_fd = fd;
    inet_ntop(AF_INET, &client_addr.sin_addr, self->client_ip,
             sizeof(self->client_ip));

    // Best-effort: lets the OS/network stack notice a peer that vanishes
    // without a clean FIN/RST sooner than SSHD_IDLE_TIMEOUT_MS. Plain
    // SOL_SOCKET option only -- the IPPROTO_TCP-level tuning knobs
    // haven't been verified against vendor source, so this is
    // deliberately conservative. Ignoring failure is fine either way:
    // SSHD_IDLE_TIMEOUT_MS above is the real backstop.
    {
      int opt = 1;
      setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
    }

    sshd_handle_client(self); // blocks for the whole session

    close(self->client_fd);
    self->client_fd = -1;
    if (!self->stop_request) {
      self->state = SSHD_STATE_LISTENING;
    }
  }

done:
  self->state = SSHD_STATE_STOPPED;
  if (self->listen_fd >= 0) {
    close(self->listen_fd);
    self->listen_fd = -1;
  }
  self->task = NULL;
  vTaskDelete(NULL);
}

// Generates a fresh ECDSA P-256 host key (DER/ASN.1-encoded, usable
// directly with wolfSSH_CTX_UsePrivateKey_buffer(...,
// WOLFSSH_FORMAT_ASN1)). Pure computation, no file I/O -- persistence
// is sshd.py's job, matching this project's convention of doing file
// I/O in Python (loracfg.py's /flash/.radio.json, etc.).
static mp_obj_t sshd_make_host_key(void) {
  static bool wolfssh_lib_initialized = false;
  if (!wolfssh_lib_initialized) {
    wolfSSH_Init();
    wolfssh_lib_initialized = true;
  }

  byte buf[SSHD_HOST_KEY_MAX_SZ];
  int sz = wolfSSH_MakeEcdsaKey(buf, sizeof(buf), WOLFSSH_ECDSAKEY_PRIME256);
  if (sz <= 0) {
    mp_raise_msg(&mp_type_RuntimeError,
                MP_ERROR_TEXT("failed to generate host key"));
  }
  return mp_obj_new_bytes(buf, (size_t)sz);
}
static MP_DEFINE_CONST_FUN_OBJ_0(sshd_make_host_key_obj, sshd_make_host_key);

// --- Constructor: sshd.Server(kvm, username, password, host_key, port) ---

static mp_obj_t sshd_server_make_new(const mp_obj_type_t *type, size_t n_args,
                                     size_t n_kw, const mp_obj_t *args) {
  mp_arg_check_num(n_args, n_kw, 5, 5, false);

  sshd_server_obj_t *self = m_new_obj(sshd_server_obj_t);
  self->base.type = type;

  self->kvm = args[0];
  self->notify_cb = mp_const_none;

  const char *username = mp_obj_str_get_str(args[1]);
  const char *password = mp_obj_str_get_str(args[2]);
  strncpy(self->username, username, sizeof(self->username) - 1);
  self->username[sizeof(self->username) - 1] = '\0';
  strncpy(self->password, password, sizeof(self->password) - 1);
  self->password[sizeof(self->password) - 1] = '\0';

  mp_buffer_info_t key_buf;
  mp_get_buffer_raise(args[3], &key_buf, MP_BUFFER_READ);
  if (key_buf.len > SSHD_HOST_KEY_MAX_SZ) {
    mp_raise_ValueError(MP_ERROR_TEXT("host key too large"));
  }
  memcpy(self->host_key, key_buf.buf, key_buf.len);
  self->host_key_sz = (word32)key_buf.len;

  self->port = mp_obj_get_int(args[4]);

  self->task = NULL;
  self->stack = NULL;
  self->state = SSHD_STATE_IDLE;
  self->stop_request = false;
  self->listen_fd = -1;
  self->client_fd = -1;
  self->client_ip[0] = '\0';
  self->last_error = 0;

  if (!rb_init(&self->tx, SSHD_RB_SIZE)) {
    mp_raise_msg(&mp_type_MemoryError,
                MP_ERROR_TEXT("failed to allocate sshd ring buffer"));
  }

  return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t sshd_server_start(mp_obj_t self_in) {
  sshd_server_obj_t *self = MP_OBJ_TO_PTR(self_in);

  if (self->task != NULL) {
    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("already started"));
  }

  // Reclaim the previous run's PSRAM stack, if any -- self->task == NULL
  // (just confirmed above) means that task already reached its own
  // "self->task = NULL" line and won't touch this memory again, same
  // reasoning as modsftpd.c's session-slot reclaim.
  if (self->stack != NULL) {
    heap_caps_free(self->stack);
    self->stack = NULL;
  }

  self->stop_request = false;
  self->last_error = 0;
  self->listen_fd = -1;
  self->client_fd = -1;
  self->state = SSHD_STATE_IDLE;
  rb_reset(&self->tx);

  // Attach as env.kvm's output mirror for this Server's whole lifetime,
  // not per-connection. Safe here: start() runs on mp_task, ordinary
  // Python method dispatch.
  {
    mp_obj_t dest[3];
    mp_load_method(self->kvm, MP_QSTR_set_mirror, dest);
    dest[2] = self_in;
    mp_call_method_n_kw(1, 0, dest);
  }

  // PSRAM-backed stack -- see modssh.c's ssh_client_connect() for the
  // full rationale.
  StackType_t *stack = heap_caps_malloc(SSHD_TASK_STACK_WORDS * sizeof(StackType_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (stack == NULL) {
    stack = heap_caps_malloc(SSHD_TASK_STACK_WORDS * sizeof(StackType_t),
                             MALLOC_CAP_8BIT);
  }
  if (stack == NULL) {
    mp_raise_msg(&mp_type_RuntimeError,
                MP_ERROR_TEXT("failed to allocate sshd task stack"));
  }
  self->stack = stack;

  TaskHandle_t task = xTaskCreateStaticPinnedToCore(
      sshd_task, "sshd", SSHD_TASK_STACK_WORDS, self, SSHD_TASK_PRIORITY,
      stack, &self->task_buf,
      1 /* APP CPU -- leave PRO CPU/core 0 for MicroPython */);

  if (task == NULL) {
    heap_caps_free(stack);
    self->stack = NULL;
    mp_raise_msg(&mp_type_RuntimeError,
                MP_ERROR_TEXT("failed to start sshd task"));
  }
  self->task = task;

  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(sshd_server_start_obj, sshd_server_start);

static mp_obj_t sshd_server_stop(mp_obj_t self_in) {
  sshd_server_obj_t *self = MP_OBJ_TO_PTR(self_in);
  self->stop_request = true;

  mp_obj_t dest[3];
  mp_load_method(self->kvm, MP_QSTR_set_mirror, dest);
  dest[2] = mp_const_none;
  mp_call_method_n_kw(1, 0, dest);

  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(sshd_server_stop_obj, sshd_server_stop);

static mp_obj_t sshd_server_status(mp_obj_t self_in) {
  sshd_server_obj_t *self = MP_OBJ_TO_PTR(self_in);
  return mp_obj_new_int(self->state);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sshd_server_status_obj, sshd_server_status);

static mp_obj_t sshd_server_error_code(mp_obj_t self_in) {
  sshd_server_obj_t *self = MP_OBJ_TO_PTR(self_in);
  return mp_obj_new_int(self->last_error);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sshd_server_error_code_obj, sshd_server_error_code);

// Registers a callback invoked as callback(connected) -- True right
// after a client authenticates, False right after that session ends.
// Pass None to clear.
//
// The background task must never touch MicroPython's heap/GC/VM state
// directly (see this file's header comment). mp_sched_schedule() is
// MicroPython's thread/ISR-safe bridge for exactly this: it enqueues
// two already-existing mp_obj_t references into a fixed-depth ring
// (MICROPY_SCHEDULER_DEPTH), no allocation, drained on mp_task at its
// own next safe point. That's why the scheduled arg is always
// mp_const_true/mp_const_false rather than a freshly built object --
// the client's IP is instead exposed via last_client_ip(), read back on
// mp_task inside the callback.
static mp_obj_t sshd_server_set_notify(mp_obj_t self_in, mp_obj_t cb) {
  sshd_server_obj_t *self = MP_OBJ_TO_PTR(self_in);
  self->notify_cb = cb;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sshd_server_set_notify_obj, sshd_server_set_notify);

// Only meaningful from within (or after) a set_notify() callback --
// mp_obj_new_str() allocates, so this must only be called from mp_task.
static mp_obj_t sshd_server_last_client_ip(mp_obj_t self_in) {
  sshd_server_obj_t *self = MP_OBJ_TO_PTR(self_in);
  return mp_obj_new_str(self->client_ip, strlen(self->client_ip));
}
static MP_DEFINE_CONST_FUN_OBJ_1(sshd_server_last_client_ip_obj, sshd_server_last_client_ip);

// Stream protocol: write-only. Its only purpose is being attachable as
// env.kvm's mirror target -- nothing ever reads from a Server.

static mp_uint_t sshd_server_write(mp_obj_t self_in, const void *buf,
                                   mp_uint_t size, int *errcode) {
  sshd_server_obj_t *self = MP_OBJ_TO_PTR(self_in);
  rb_write(&self->tx, (const uint8_t *)buf, size);
  // Always report the full size as "written" -- a full ring buffer
  // should silently drop the overflow rather than surface as an error.
  return size;
}

static const mp_stream_p_t sshd_server_stream_p = {
    .write = sshd_server_write,
    .is_text = false,
};

static const mp_rom_map_elem_t sshd_server_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&sshd_server_start_obj)},
    {MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&sshd_server_stop_obj)},
    {MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&sshd_server_status_obj)},
    {MP_ROM_QSTR(MP_QSTR_error_code), MP_ROM_PTR(&sshd_server_error_code_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_notify), MP_ROM_PTR(&sshd_server_set_notify_obj)},
    {MP_ROM_QSTR(MP_QSTR_last_client_ip), MP_ROM_PTR(&sshd_server_last_client_ip_obj)},
    {MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj)},
};
static MP_DEFINE_CONST_DICT(sshd_server_locals_dict, sshd_server_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(sshd_server_type, MP_QSTR_Server, MP_TYPE_FLAG_NONE,
                         make_new, sshd_server_make_new, protocol,
                         &sshd_server_stream_p, locals_dict,
                         &sshd_server_locals_dict);

static const mp_rom_map_elem_t modsshd_module_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_modsshd)},
    {MP_ROM_QSTR(MP_QSTR_Server), MP_ROM_PTR(&sshd_server_type)},
    {MP_ROM_QSTR(MP_QSTR_make_host_key), MP_ROM_PTR(&sshd_make_host_key_obj)},
    {MP_ROM_QSTR(MP_QSTR_IDLE), MP_ROM_INT(SSHD_STATE_IDLE)},
    {MP_ROM_QSTR(MP_QSTR_LISTENING), MP_ROM_INT(SSHD_STATE_LISTENING)},
    {MP_ROM_QSTR(MP_QSTR_CONNECTED), MP_ROM_INT(SSHD_STATE_CONNECTED)},
    {MP_ROM_QSTR(MP_QSTR_STOPPED), MP_ROM_INT(SSHD_STATE_STOPPED)},
};
static MP_DEFINE_CONST_DICT(modsshd_module_globals, modsshd_module_globals_table);

const mp_obj_module_t modsshd_module = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&modsshd_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_modsshd, modsshd_module);
