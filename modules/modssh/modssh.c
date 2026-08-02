/*
 * MicroPython SSH client (wolfSSH-backed)
 * Copyright (c) 2026 8bitmcu
 * License: MIT
 *
 * Password and keyboard-interactive auth only, no host key verification,
 * client mode only.
 *
 * wolfSSH's handshake and steady-state read/write run on a dedicated
 * FreeRTOS task pinned to core 1, same pattern as
 * tdeck_i2s/audioplayer.c's decode task -- keeps mp_task free and avoids
 * tripping the watchdog with wolfCrypt's heavier operations.
 *
 * Data crosses the core boundary through two ring_buf_t instances (same
 * primitive as audioplayer.c/audiorecorder.c). The task never touches
 * MicroPython's heap/GC directly -- only raw bytes through the ring
 * buffers.
 */

#include "py/obj.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "py/mphal.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// xTaskCreatePinnedToCore moved here in ESP-IDF 5.3+ (upstream FreeRTOS SMP
// merge); see modules/tdeck_i2s/audioplayer.c for the same pattern.
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h" // heap_caps_malloc(MALLOC_CAP_SPIRAM), see modules/modc2/modc2_alloc.c

#include "ring_buf.h"

#include <wolfssh/ssh.h>
#include <wolfssh/error.h>
#include <wolfssh/settings.h> // WOLFSSH_MAX_PROMPTS

#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

// Stack is in WORDS here, not bytes -- 16384 words is ~64KB, headroom for
// wolfCrypt's ECC work plus keyboard-interactive auth.
#define SSH_TASK_STACK_WORDS 16384
#define SSH_TASK_PRIORITY 5

// Matches wolfSSL's DEFAULT_WINDOW_SZ (2000 bytes) -- no point buffering
// more than the SSH window allows in flight.
#define SSH_RB_SIZE 4096

// select() timeout for the steady-state loop -- NOT a socket-level
// SO_RCVTIMEO. That makes wolfSSH's own recv() return EAGAIN, which
// wolfSSH_stream_read() mishandles (returns WS_ERROR instead of
// WS_WANT_READ, killing the session). This is a plain select() on our
// own fd outside wolfSSH's control, so it's safe to keep short.
#define SSH_RECV_TIMEOUT_MS 100

// Bounds for the pre-connected phase (see ssh_wait_fd()).
#define SSH_CONNECT_POLL_MS 200
#define SSH_CONNECT_TIMEOUT_MS 10000
#define SSH_HANDSHAKE_TIMEOUT_MS 10000

typedef enum {
  SSH_STATE_IDLE = 0,
  SSH_STATE_CONNECTING,
  SSH_STATE_CONNECTED,
  SSH_STATE_FAILED,
  SSH_STATE_CLOSED,
} ssh_state_t;

typedef struct _ssh_client_obj_t {
  mp_obj_base_t base;

  TaskHandle_t task;
  StaticTask_t task_buf; // xTaskCreateStaticPinnedToCore()'s control block
  StackType_t *stack;    // PSRAM-backed, see ssh_client_connect()
  volatile ssh_state_t state;
  volatile bool stop_request;

  ring_buf_t rx; // SSH task (producer) -> MicroPython (consumer)
  ring_buf_t tx; // MicroPython (producer) -> SSH task (consumer)

  // Copied rather than referencing the Python str objects -- must stay
  // valid independent of MicroPython's GC.
  char host[64];
  int port;
  char username[32];
  char password[64];

  int sockfd;
  WOLFSSH_CTX *ctx;
  WOLFSSH *ssh;

  // Diagnostic only, read from Python once status() reports FAILED.
  // SSH_ERR_* below cover failures before a WOLFSSH* exists; otherwise
  // this holds the real WS_* code from wolfssh/error.h.
  int last_error;

  // Diagnostic only, set from ssh_user_auth().
  int auth_attempts;
  int last_auth_type;

  // Scratch space for WOLFSSH_USERAUTH_KEYBOARD prompts -- sized to
  // WOLFSSH_MAX_PROMPTS (wolfSSH's own hard cap, enforced before our
  // callback runs). Per-instance so concurrent Clients don't clobber
  // each other.
  byte *kb_responses[WOLFSSH_MAX_PROMPTS];
  word32 kb_response_lengths[WOLFSSH_MAX_PROMPTS];
} ssh_client_obj_t;

// wolfssh/error.h's WS_* codes run continuously from -1 to at least
// -1097 -- these sentinels stay well clear of that whole span.
#define SSH_ERR_DNS -2000
#define SSH_ERR_SOCKET -2001
#define SSH_ERR_CONNECT -2002
#define SSH_ERR_CTX_NEW -2003
#define SSH_ERR_SSH_NEW -2004
#define SSH_ERR_CONNECT_TIMEOUT -2005
#define SSH_ERR_HANDSHAKE_TIMEOUT -2006
#define SSH_ERR_ABORTED -2007

const mp_obj_type_t ssh_client_type;

// wolfSSH_SetUserAuth callback. Handles both WOLFSSH_USERAUTH_PASSWORD
// and WOLFSSH_USERAUTH_KEYBOARD: once a server offers keyboard-
// interactive alongside password (common with PAM-backed OpenSSH),
// wolfSSH's client unconditionally prefers it with no fallback, so a
// password-only callback would never get asked for PASSWORD. Answering
// every keyboard prompt with the stored password covers the common case
// (a single "Password:" prompt).
static int ssh_user_auth(byte authType, WS_UserAuthData *authData, void *ctx) {
  ssh_client_obj_t *self = (ssh_client_obj_t *)ctx;
  self->auth_attempts++;
  self->last_auth_type = (int)authType;

  if (authType == WOLFSSH_USERAUTH_PASSWORD) {
    authData->sf.password.password = (byte *)self->password;
    authData->sf.password.passwordSz = (word32)strlen(self->password);
    return WOLFSSH_USERAUTH_SUCCESS;
  }

  if (authType == WOLFSSH_USERAUTH_KEYBOARD) {
    // DoUserAuthInfoRequest() already rejects more than
    // WOLFSSH_MAX_PROMPTS prompts before this callback runs.
    word32 count = authData->sf.keyboard.promptCount;
    word32 passwordSz = (word32)strlen(self->password);
    for (word32 i = 0; i < count; i++) {
      self->kb_responses[i] = (byte *)self->password;
      self->kb_response_lengths[i] = passwordSz;
    }
    authData->sf.keyboard.responseCount = count;
    authData->sf.keyboard.responses = self->kb_responses;
    authData->sf.keyboard.responseLengths = self->kb_response_lengths;
    return WOLFSSH_USERAUTH_SUCCESS;
  }

  return WOLFSSH_USERAUTH_FAILURE;
}

// Waits up to timeout_ms for `fd` to become ready (writable if
// for_write, readable otherwise), polling in short increments so
// self->stop_request is noticed promptly instead of only after a long
// select() returns. Used to bound and make interruptible the
// pre-connected phase (connect() and waiting for the server's first
// byte), which previously had no bound and ignored stop_request
// entirely.
//
// Returns 1 if the fd became ready, 0 on timeout, -1 if stop_request
// fired first.
static int ssh_wait_fd(ssh_client_obj_t *self, int fd, bool for_write,
                       int timeout_ms) {
  int waited = 0;
  while (waited < timeout_ms) {
    if (self->stop_request) {
      return -1;
    }
    int poll_ms = SSH_CONNECT_POLL_MS;
    if (timeout_ms - waited < poll_ms) {
      poll_ms = timeout_ms - waited;
    }
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv;
    tv.tv_sec = poll_ms / 1000;
    tv.tv_usec = (poll_ms % 1000) * 1000;
    int sel = for_write ? select(fd + 1, NULL, &fds, NULL, &tv)
                        : select(fd + 1, &fds, NULL, NULL, &tv);
    if (sel > 0 && FD_ISSET(fd, &fds)) {
      return 1;
    }
    waited += poll_ms;
  }
  return 0;
}

static void ssh_task(void *arg) {
  ssh_client_obj_t *self = (ssh_client_obj_t *)arg;

  static bool wolfssh_lib_initialized = false;
  if (!wolfssh_lib_initialized) {
    wolfSSH_Init();
    wolfssh_lib_initialized = true;
  }

  self->sockfd = -1;
  self->ctx = NULL;
  self->ssh = NULL;

  if (self->stop_request) {
    self->state = SSH_STATE_CLOSED;
    goto done;
  }

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  char port_str[6];
  snprintf(port_str, sizeof(port_str), "%d", self->port);

  // getaddrinfo() is blocking with no portable way to bound/interrupt
  // it -- known residual gap, unlike connect()/wolfSSH_connect() below.
  struct addrinfo *res = NULL;
  if (getaddrinfo(self->host, port_str, &hints, &res) != 0 || res == NULL) {
    self->last_error = SSH_ERR_DNS;
    self->state = SSH_STATE_FAILED;
    goto done;
  }

  self->sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (self->sockfd < 0) {
    freeaddrinfo(res);
    self->last_error = SSH_ERR_SOCKET;
    self->state = SSH_STATE_FAILED;
    goto done;
  }

  // Non-blocking connect(), bounded via ssh_wait_fd(); restored to
  // blocking before wolfSSH touches the fd -- wolfSSH_connect() (like
  // wolfSSH_accept(), see modsshd.c) treats WS_WANT_READ from a
  // non-blocking recv() as fatal, so it needs a blocking socket
  // throughout.
  int sock_flags = fcntl(self->sockfd, F_GETFL, 0);
  fcntl(self->sockfd, F_SETFL, sock_flags | O_NONBLOCK);

  int connect_rc = connect(self->sockfd, res->ai_addr, res->ai_addrlen);
  freeaddrinfo(res);

  if (connect_rc != 0 && errno != EINPROGRESS) {
    self->last_error = SSH_ERR_CONNECT;
    self->state = SSH_STATE_FAILED;
    goto done;
  }

  if (connect_rc != 0) {
    int w = ssh_wait_fd(self, self->sockfd, true, SSH_CONNECT_TIMEOUT_MS);
    if (w < 0) {
      self->last_error = SSH_ERR_ABORTED;
      self->state = SSH_STATE_CLOSED;
      goto done;
    }
    if (w == 0) {
      self->last_error = SSH_ERR_CONNECT_TIMEOUT;
      self->state = SSH_STATE_FAILED;
      goto done;
    }
    int so_err = 0;
    socklen_t so_err_len = sizeof(so_err);
    getsockopt(self->sockfd, SOL_SOCKET, SO_ERROR, &so_err, &so_err_len);
    if (so_err != 0) {
      self->last_error = SSH_ERR_CONNECT;
      self->state = SSH_STATE_FAILED;
      goto done;
    }
  }

  fcntl(self->sockfd, F_SETFL, sock_flags); // restore blocking

  self->ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_CLIENT, NULL);
  if (self->ctx == NULL) {
    self->last_error = SSH_ERR_CTX_NEW;
    self->state = SSH_STATE_FAILED;
    goto done;
  }

  wolfSSH_SetUserAuth(self->ctx, ssh_user_auth);

  self->ssh = wolfSSH_new(self->ctx);
  if (self->ssh == NULL) {
    self->last_error = SSH_ERR_SSH_NEW;
    self->state = SSH_STATE_FAILED;
    goto done;
  }

  wolfSSH_SetUserAuthCtx(self->ssh, (void *)self);
  wolfSSH_set_fd(self->ssh, self->sockfd);
  wolfSSH_SetUsername(self->ssh, self->username);

  // Interactive shell/PTY. Must happen BEFORE wolfSSH_connect() --
  // wolfSSH_connect()'s state machine checks ssh->sendTerminalRequest
  // partway through, so setting this after connect() returns is too
  // late (sends a bare "shell" request with no pty-req).
  wolfSSH_SetChannelType(self->ssh, WOLFSSH_SESSION_TERMINAL, NULL, 0);

  // Bound how long we wait for the server's first byte -- wolfSSH_connect()
  // treats any internal WS_WANT_READ as fatal with no retry, so this can
  // only be a pre-call gate, never a non-blocking retry loop around the
  // call itself (mirrors modsshd.c's identical gate before
  // wolfSSH_accept()).
  {
    int w = ssh_wait_fd(self, self->sockfd, false, SSH_HANDSHAKE_TIMEOUT_MS);
    if (w < 0) {
      self->last_error = SSH_ERR_ABORTED;
      self->state = SSH_STATE_CLOSED;
      goto done;
    }
    if (w == 0) {
      self->last_error = SSH_ERR_HANDSHAKE_TIMEOUT;
      self->state = SSH_STATE_FAILED;
      goto done;
    }
  }

  {
    int rc = wolfSSH_connect(self->ssh);
    if (rc != WS_SUCCESS) {
      self->last_error = wolfSSH_get_error(self->ssh);
      self->state = SSH_STATE_FAILED;
      goto done;
    }
  }

  self->state = SSH_STATE_CONNECTED;

  // Steady state: no SO_RCVTIMEO (see SSH_RECV_TIMEOUT_MS above) --
  // select() gates whether we call into wolfSSH at all, so its own
  // recv() only ever runs when data is already available.
  uint8_t iobuf[256];
  while (!self->stop_request) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(self->sockfd, &rfds);
    struct timeval tv;
    tv.tv_sec = SSH_RECV_TIMEOUT_MS / 1000;
    tv.tv_usec = (SSH_RECV_TIMEOUT_MS % 1000) * 1000;
    int sel = select(self->sockfd + 1, &rfds, NULL, NULL, &tv);

    if (sel > 0 && FD_ISSET(self->sockfd, &rfds)) {
      int n = wolfSSH_stream_read(self->ssh, iobuf, sizeof(iobuf));
      if (n > 0) {
        rb_write(&self->rx, iobuf, (size_t)n);
      } else if (n != WS_WANT_READ) {
        // WS_EOF / WS_CHANNEL_CLOSED / WS_DISCONNECT / any other error.
        self->last_error = wolfSSH_get_error(self->ssh);
        break;
      }
    }

    size_t avail = rb_available(&self->tx);
    if (avail > 0) {
      size_t chunk = avail < sizeof(iobuf) ? avail : sizeof(iobuf);
      size_t got = rb_read(&self->tx, iobuf, chunk);
      if (got > 0) {
        wolfSSH_stream_send(self->ssh, iobuf, (word32)got);
      }
    }
  }

  self->state = SSH_STATE_CLOSED;

done:
  if (self->ssh) {
    wolfSSH_shutdown(self->ssh);
    wolfSSH_free(self->ssh);
    self->ssh = NULL;
  }
  if (self->ctx) {
    wolfSSH_CTX_free(self->ctx);
    self->ctx = NULL;
  }
  if (self->sockfd >= 0) {
    close(self->sockfd);
    self->sockfd = -1;
  }
  self->task = NULL;
  vTaskDelete(NULL);
}

static mp_obj_t ssh_client_make_new(const mp_obj_type_t *type, size_t n_args,
                                    size_t n_kw, const mp_obj_t *args) {
  ssh_client_obj_t *self = m_new_obj(ssh_client_obj_t);
  self->base.type = type;
  self->task = NULL;
  self->stack = NULL;
  self->state = SSH_STATE_IDLE;
  self->stop_request = false;
  self->sockfd = -1;
  self->ctx = NULL;
  self->ssh = NULL;
  self->last_error = 0;
  self->auth_attempts = 0;
  self->last_auth_type = -1;

  if (!rb_init(&self->rx, SSH_RB_SIZE) || !rb_init(&self->tx, SSH_RB_SIZE)) {
    mp_raise_msg(&mp_type_MemoryError,
                MP_ERROR_TEXT("failed to allocate ssh ring buffers"));
  }

  return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t ssh_client_connect(size_t n_args, const mp_obj_t *args) {
  ssh_client_obj_t *self = MP_OBJ_TO_PTR(args[0]);

  // self->task != NULL also guards here (not just state): ssh_task() sets
  // state to FAILED/CLOSED a few lines before its own cleanup (closing
  // ctx/ssh/sockfd) finishes and self->task is nulled -- without this, a
  // connect() landing in that narrow window would start a second task
  // while the first is still tearing down the very fields this one is
  // about to reinitialize.
  if (self->state == SSH_STATE_CONNECTING || self->state == SSH_STATE_CONNECTED ||
      self->task != NULL) {
    mp_raise_msg(&mp_type_RuntimeError,
                MP_ERROR_TEXT("already connecting or connected"));
  }

  // Reclaim the previous run's PSRAM stack, if any -- self->task == NULL
  // (just confirmed above) means that task already reached its own
  // "self->task = NULL" line and won't touch this memory again, same
  // reasoning as modsftpd.c's session-slot reclaim.
  if (self->stack != NULL) {
    heap_caps_free(self->stack);
    self->stack = NULL;
  }

  const char *host = mp_obj_str_get_str(args[1]);
  mp_int_t port = mp_obj_get_int(args[2]);
  const char *username = mp_obj_str_get_str(args[3]);
  const char *password = mp_obj_str_get_str(args[4]);

  strncpy(self->host, host, sizeof(self->host) - 1);
  self->host[sizeof(self->host) - 1] = '\0';
  self->port = (int)port;
  strncpy(self->username, username, sizeof(self->username) - 1);
  self->username[sizeof(self->username) - 1] = '\0';
  strncpy(self->password, password, sizeof(self->password) - 1);
  self->password[sizeof(self->password) - 1] = '\0';

  rb_reset(&self->rx);
  rb_reset(&self->tx);
  self->stop_request = false;
  self->last_error = 0;
  self->auth_attempts = 0;
  self->last_auth_type = -1;
  self->state = SSH_STATE_CONNECTING;

  // Stack allocated from PSRAM, falling back to internal RAM if PSRAM's
  // exhausted -- same pattern as modsftpd.c's per-session task stacks.
  // 64KB of internal RAM per SSH session was a real squeeze on this
  // board's scarce internal pool (see mpconfigboard.h's WiFi/BT headroom
  // investigation).
  StackType_t *stack = heap_caps_malloc(SSH_TASK_STACK_WORDS * sizeof(StackType_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (stack == NULL) {
    stack = heap_caps_malloc(SSH_TASK_STACK_WORDS * sizeof(StackType_t),
                             MALLOC_CAP_8BIT);
  }
  if (stack == NULL) {
    self->state = SSH_STATE_FAILED;
    mp_raise_msg(&mp_type_RuntimeError,
                MP_ERROR_TEXT("failed to allocate ssh task stack"));
  }
  self->stack = stack;

  TaskHandle_t task = xTaskCreateStaticPinnedToCore(
      ssh_task, "sshclient", SSH_TASK_STACK_WORDS, self, SSH_TASK_PRIORITY,
      stack, &self->task_buf,
      1 /* APP CPU -- leave PRO CPU/core 0 for MicroPython */);

  if (task == NULL) {
    heap_caps_free(stack);
    self->stack = NULL;
    self->state = SSH_STATE_FAILED;
    mp_raise_msg(&mp_type_RuntimeError,
                MP_ERROR_TEXT("failed to start ssh task"));
  }
  self->task = task;

  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ssh_client_connect_obj, 5, 5,
                                           ssh_client_connect);

static mp_obj_t ssh_client_status(mp_obj_t self_in) {
  ssh_client_obj_t *self = MP_OBJ_TO_PTR(self_in);
  return mp_obj_new_int(self->state);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ssh_client_status_obj, ssh_client_status);

static mp_obj_t ssh_client_error_code(mp_obj_t self_in) {
  ssh_client_obj_t *self = MP_OBJ_TO_PTR(self_in);
  return mp_obj_new_int(self->last_error);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ssh_client_error_code_obj, ssh_client_error_code);

static mp_obj_t ssh_client_auth_attempts(mp_obj_t self_in) {
  ssh_client_obj_t *self = MP_OBJ_TO_PTR(self_in);
  return mp_obj_new_int(self->auth_attempts);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ssh_client_auth_attempts_obj, ssh_client_auth_attempts);

static mp_obj_t ssh_client_last_auth_type(mp_obj_t self_in) {
  ssh_client_obj_t *self = MP_OBJ_TO_PTR(self_in);
  return mp_obj_new_int(self->last_auth_type);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ssh_client_last_auth_type_obj, ssh_client_last_auth_type);

static mp_obj_t ssh_client_disconnect(mp_obj_t self_in) {
  ssh_client_obj_t *self = MP_OBJ_TO_PTR(self_in);
  self->stop_request = true;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ssh_client_disconnect_obj, ssh_client_disconnect);

// Stream protocol: readinto() drains decrypted output, write() queues
// keystrokes -- both just move bytes through the ring buffers.

static mp_uint_t ssh_client_read(mp_obj_t self_in, void *buf, mp_uint_t size,
                                 int *errcode) {
  ssh_client_obj_t *self = MP_OBJ_TO_PTR(self_in);
  size_t n = rb_read(&self->rx, (uint8_t *)buf, size);
  if (n == 0) {
    *errcode = MP_EAGAIN;
    return MP_STREAM_ERROR;
  }
  return n;
}

static mp_uint_t ssh_client_write(mp_obj_t self_in, const void *buf,
                                  mp_uint_t size, int *errcode) {
  ssh_client_obj_t *self = MP_OBJ_TO_PTR(self_in);
  size_t n = rb_write(&self->tx, (const uint8_t *)buf, size);
  if (n == 0) {
    *errcode = MP_EAGAIN;
    return MP_STREAM_ERROR;
  }
  return n;
}

static mp_uint_t ssh_client_ioctl(mp_obj_t self_in, mp_uint_t request,
                                  uintptr_t arg, int *errcode) {
  ssh_client_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (request == MP_STREAM_POLL) {
    uintptr_t flags = arg;
    uintptr_t ret = 0;
    if ((flags & MP_STREAM_POLL_RD) && rb_available(&self->rx) > 0) {
      ret |= MP_STREAM_POLL_RD;
    }
    if ((flags & MP_STREAM_POLL_WR) && rb_free_space(&self->tx) > 0) {
      ret |= MP_STREAM_POLL_WR;
    }
    return ret;
  }
  return 0;
}

static const mp_stream_p_t ssh_client_stream_p = {
    .read = ssh_client_read,
    .write = ssh_client_write,
    .ioctl = ssh_client_ioctl,
    .is_text = false,
};

static const mp_rom_map_elem_t ssh_client_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&ssh_client_connect_obj)},
    {MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&ssh_client_status_obj)},
    {MP_ROM_QSTR(MP_QSTR_error_code), MP_ROM_PTR(&ssh_client_error_code_obj)},
    {MP_ROM_QSTR(MP_QSTR_auth_attempts), MP_ROM_PTR(&ssh_client_auth_attempts_obj)},
    {MP_ROM_QSTR(MP_QSTR_last_auth_type), MP_ROM_PTR(&ssh_client_last_auth_type_obj)},
    {MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&ssh_client_disconnect_obj)},
    {MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj)},
    {MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj)},
    {MP_ROM_QSTR(MP_QSTR_ioctl), MP_ROM_PTR(&mp_stream_ioctl_obj)},
};
static MP_DEFINE_CONST_DICT(ssh_client_locals_dict, ssh_client_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(ssh_client_type, MP_QSTR_Client,
                         MP_TYPE_FLAG_ITER_IS_STREAM, make_new,
                         ssh_client_make_new, protocol, &ssh_client_stream_p,
                         locals_dict, &ssh_client_locals_dict);

static const mp_rom_map_elem_t modssh_module_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_modssh)},
    {MP_ROM_QSTR(MP_QSTR_Client), MP_ROM_PTR(&ssh_client_type)},
    {MP_ROM_QSTR(MP_QSTR_IDLE), MP_ROM_INT(SSH_STATE_IDLE)},
    {MP_ROM_QSTR(MP_QSTR_CONNECTING), MP_ROM_INT(SSH_STATE_CONNECTING)},
    {MP_ROM_QSTR(MP_QSTR_CONNECTED), MP_ROM_INT(SSH_STATE_CONNECTED)},
    {MP_ROM_QSTR(MP_QSTR_FAILED), MP_ROM_INT(SSH_STATE_FAILED)},
    {MP_ROM_QSTR(MP_QSTR_CLOSED), MP_ROM_INT(SSH_STATE_CLOSED)},
};
static MP_DEFINE_CONST_DICT(modssh_module_globals, modssh_module_globals_table);

const mp_obj_module_t modssh_module = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&modssh_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_modssh, modssh_module);
