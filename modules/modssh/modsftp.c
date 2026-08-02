/*
 * MicroPython SFTP client (wolfSSH-backed)
 * Copyright (c) 2026 8bitmcu
 * License: MIT
 *
 * A dedicated WOLFSSH_SESSION_SUBSYSTEM "sftp" connection, separate from
 * modssh.c's interactive shell client -- wolfSSH_SFTP_connect() wraps
 * wolfSSH_connect() with a fixed channel type, so a session can't be
 * both. Backs modules/scripts/applications/sftp.py's VFS mount, the same
 * way ftp.py's FTP_VFS drives a plain socket.
 *
 * Never calls wolfSSH_SFTP_Get()/_Put() -- they shell out to WFOPEN()/
 * WFWRITE()/WFREAD() on a *local* file, which this project's NO_FILESYSTEM
 * setting leaves undefined (see micropython.cmake's stub defines). The
 * low-level primitives used here (Open/SendReadPacket/SendWritePacket/
 * Close/LS/STAT/etc.) don't touch those macros at all -- they just hand
 * raw bytes to/from the caller, which sftp.py turns into VFS file I/O.
 *
 * SFTP is request/response, not a continuous stream, and MicroPython's
 * VFS protocol is synchronous by contract -- so instead of modssh.c's
 * ring_buf_t pair, each Client method is a blocking RPC across the task
 * boundary: fill in the shared request struct, signal the background
 * task, block until it signals back. Only one request is ever in flight,
 * so the two semaphores are enough on their own (no separate mutex) --
 * ownership of the struct is always exclusively on one side. The
 * blocking wait on mp_task's side yields the CPU (unlike a busy loop),
 * so the actual crypto-heavy wolfSSH_SFTP_* call still only ever runs on
 * the background task's stack, preserving the same watchdog-safety
 * property modssh.c's header comment describes.
 */

#include "py/obj.h"
#include "py/runtime.h"
#include "py/mperrno.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h" // xTaskCreatePinnedToCore, see modssh.c
#include "esp_heap_caps.h" // heap_caps_malloc(MALLOC_CAP_SPIRAM), see modules/modc2/modc2_alloc.c

#include <wolfssh/ssh.h>
#include <wolfssh/error.h>
#include <wolfssh/wolfsftp.h>
#include <wolfssh/settings.h> // WOLFSSH_MAX_PROMPTS

#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

#define SFTP_TASK_STACK_WORDS 16384
#define SFTP_TASK_PRIORITY 5

#define SFTP_CONNECT_POLL_MS 200
#define SFTP_CONNECT_TIMEOUT_MS 10000
#define SFTP_HANDSHAKE_TIMEOUT_MS 10000

// Poll interval for the request/response loop's wait on request_sem --
// short enough that stop_request is noticed promptly with no request
// pending, same idea as modsshd.c's SSHD_ACCEPT_POLL_MS.
#define SFTP_REQUEST_POLL_MS 200

// How long mp_task will wait for one RPC to complete before giving up --
// bounds a stuck operation (e.g. the connection dies mid-transfer)
// rather than hanging the whole device forever. The background task
// itself isn't guaranteed to unstick within this window -- same
// known-residual-orphan-task tradeoff as modssh.c's connect-phase fix,
// just not fully closed here for every possible stuck op.
#define SFTP_OP_TIMEOUT_MS 30000

#define SFTP_PATH_MAX 256
#define SFTP_SCRATCH_SZ 4096 // read/write chunk size, well under WOLFSSH_MAX_SFTP_RW

typedef enum {
  SFTP_STATE_IDLE = 0,
  SFTP_STATE_CONNECTING,
  SFTP_STATE_CONNECTED,
  SFTP_STATE_FAILED,
  SFTP_STATE_CLOSED,
} sftp_state_t;

typedef enum {
  SFTP_OP_LS = 1,
  SFTP_OP_STAT,
  SFTP_OP_LSTAT,
  SFTP_OP_OPEN,
  SFTP_OP_READ,
  SFTP_OP_WRITE,
  SFTP_OP_CLOSE,
  SFTP_OP_MKDIR,
  SFTP_OP_RMDIR,
  SFTP_OP_REMOVE,
  SFTP_OP_RENAME,
} sftp_op_t;

typedef struct _sftp_client_obj_t {
  mp_obj_base_t base;

  TaskHandle_t task;
  StaticTask_t task_buf; // xTaskCreateStaticPinnedToCore()'s control block
  StackType_t *stack;    // PSRAM-backed, see sftp_client_connect()
  volatile sftp_state_t state;
  volatile bool stop_request;

  char host[64];
  int port;
  char username[32];
  char password[64];

  int sockfd;
  WOLFSSH_CTX *ctx;
  WOLFSSH *ssh;

  int last_error;

  byte *kb_responses[WOLFSSH_MAX_PROMPTS];
  word32 kb_response_lengths[WOLFSSH_MAX_PROMPTS];

  // Request/response RPC -- see this file's header comment.
  SemaphoreHandle_t request_sem;
  SemaphoreHandle_t response_sem;

  sftp_op_t op;
  char path_a[SFTP_PATH_MAX];
  char path_b[SFTP_PATH_MAX]; // rename() target
  word32 open_reason;         // WOLFSSH_FXF_* for OPEN
  byte handle[WOLFSSH_MAX_HANDLE];
  word32 handle_sz;
  word32 ofst[2];             // {lo, hi}, see wolfSSH_SFTP_SendReadPacket/Write
  byte scratch[SFTP_SCRATCH_SZ];
  word32 scratch_sz;          // in: bytes to write; out: bytes read
  WS_SFTP_FILEATRB atrb;      // out for STAT/LSTAT
  WS_SFTPNAME *ls_result;     // out for LS -- wolfSSH-heap allocated, caller frees
  int op_ret;
} sftp_client_obj_t;

// wolfssh/error.h's WS_* codes run continuously from -1 to at least
// -1097 -- these sentinels stay well clear of that whole span, and of
// modssh.c's/modsshd.c's own -2000/-4000 ranges.
#define SFTP_ERR_DNS -5000
#define SFTP_ERR_SOCKET -5001
#define SFTP_ERR_CONNECT -5002
#define SFTP_ERR_CTX_NEW -5003
#define SFTP_ERR_SSH_NEW -5004
#define SFTP_ERR_CONNECT_TIMEOUT -5005
#define SFTP_ERR_HANDSHAKE_TIMEOUT -5006
#define SFTP_ERR_ABORTED -5007
#define SFTP_ERR_OP_TIMEOUT -5008
#define SFTP_ERR_NOT_CONNECTED -5009

const mp_obj_type_t sftp_client_type;

// Same shape as modssh.c's ssh_user_auth() -- password + keyboard-
// interactive fallback, see that file for the full rationale.
static int sftp_user_auth(byte authType, WS_UserAuthData *authData, void *ctx) {
  sftp_client_obj_t *self = (sftp_client_obj_t *)ctx;

  if (authType == WOLFSSH_USERAUTH_PASSWORD) {
    authData->sf.password.password = (byte *)self->password;
    authData->sf.password.passwordSz = (word32)strlen(self->password);
    return WOLFSSH_USERAUTH_SUCCESS;
  }

  if (authType == WOLFSSH_USERAUTH_KEYBOARD) {
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

// Same as modssh.c's ssh_wait_fd() -- duplicated rather than shared
// across translation units, matching modsshd.c's existing precedent of
// having its own inline select() gate rather than calling into
// modssh.c. See modssh.c's ssh_wait_fd() for the full rationale.
static int sftp_wait_fd(sftp_client_obj_t *self, int fd, bool for_write,
                        int timeout_ms) {
  int waited = 0;
  while (waited < timeout_ms) {
    if (self->stop_request) {
      return -1;
    }
    int poll_ms = SFTP_CONNECT_POLL_MS;
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

// Runs on the background task: performs the one queued operation and
// fills in self->op_ret/last_error/scratch/handle/ls_result as needed.
// Always on a blocking socket (see sftp_task()), so WS_WANT_READ/
// WS_WINDOW_FULL retry handling isn't needed here -- the same reasoning
// already proven for wolfSSH_stream_read()/_send() in modssh.c/modsshd.c's
// steady-state loops applies to these SFTP primitives too, since they
// ride the same underlying packet-send/recv machinery.
//
// Known residual gap: if the peer vanishes without a clean FIN/RST, the
// wolfSSH_SFTP_* call below only returns once the OS's own TCP
// retransmission timeout gives up (can be several minutes) -- there's
// no select()-gate to bound it the way the connect phase has (that fix
// doesn't apply here: this is a single already-established blocking
// call, not a wait for the first byte of a not-yet-open connection).
// Until it returns, this task never checks stop_request again, so
// Client.disconnect() is a no-op for the rest of that window --
// SFTP_OP_TIMEOUT_MS only bounds how long mp_task waits for a response,
// not how long this task stays stuck. sftp_task()'s SO_KEEPALIVE
// shrinks the likely window but doesn't close it (same best-effort
// caveat as modsshd.c's identical use). A full fix would need the
// per-operation calls to be interruptible mid-flight, which none of
// wolfSSH's SFTP primitives support on a blocking socket -- same class
// of unresolved tradeoff as modssh.c's own documented DNS-resolution gap.
static void sftp_process_op(sftp_client_obj_t *self) {
  switch (self->op) {
    case SFTP_OP_LS:
      self->ls_result = wolfSSH_SFTP_LS(self->ssh, self->path_a);
      self->op_ret = (self->ls_result != NULL) ? WS_SUCCESS
                                                : wolfSSH_get_error(self->ssh);
      break;

    case SFTP_OP_STAT:
      self->op_ret = wolfSSH_SFTP_STAT(self->ssh, self->path_a, &self->atrb);
      break;

    case SFTP_OP_LSTAT:
      self->op_ret = wolfSSH_SFTP_LSTAT(self->ssh, self->path_a, &self->atrb);
      break;

    case SFTP_OP_OPEN:
      self->handle_sz = sizeof(self->handle);
      self->op_ret = wolfSSH_SFTP_Open(self->ssh, self->path_a,
                                       self->open_reason, NULL, self->handle,
                                       &self->handle_sz);
      break;

    case SFTP_OP_READ: {
      int n = wolfSSH_SFTP_SendReadPacket(self->ssh, self->handle,
                                          self->handle_sz, self->ofst,
                                          self->scratch, sizeof(self->scratch));
      if (n < 0) {
        self->op_ret = n;
        self->scratch_sz = 0;
      } else {
        self->op_ret = WS_SUCCESS;
        self->scratch_sz = (word32)n; // 0 == EOF
      }
      break;
    }

    case SFTP_OP_WRITE: {
      int n = wolfSSH_SFTP_SendWritePacket(self->ssh, self->handle,
                                           self->handle_sz, self->ofst,
                                           self->scratch, self->scratch_sz);
      if (n <= 0) {
        self->op_ret = (n < 0) ? n : WS_FATAL_ERROR;
        self->scratch_sz = 0;
      } else {
        self->op_ret = WS_SUCCESS;
        self->scratch_sz = (word32)n;
      }
      break;
    }

    case SFTP_OP_CLOSE:
      self->op_ret = wolfSSH_SFTP_Close(self->ssh, self->handle, self->handle_sz);
      break;

    case SFTP_OP_MKDIR:
      self->op_ret = wolfSSH_SFTP_MKDIR(self->ssh, self->path_a, NULL);
      break;

    case SFTP_OP_RMDIR:
      self->op_ret = wolfSSH_SFTP_RMDIR(self->ssh, self->path_a);
      break;

    case SFTP_OP_REMOVE:
      self->op_ret = wolfSSH_SFTP_Remove(self->ssh, self->path_a);
      break;

    case SFTP_OP_RENAME:
      self->op_ret = wolfSSH_SFTP_Rename(self->ssh, self->path_a, self->path_b);
      break;

    default:
      self->op_ret = WS_BAD_ARGUMENT;
      break;
  }

  if (self->op_ret != WS_SUCCESS) {
    self->last_error = wolfSSH_get_error(self->ssh);
  }
}

static void sftp_task(void *arg) {
  sftp_client_obj_t *self = (sftp_client_obj_t *)arg;

  static bool wolfssh_lib_initialized = false;
  if (!wolfssh_lib_initialized) {
    wolfSSH_Init();
    wolfssh_lib_initialized = true;
  }

  self->sockfd = -1;
  self->ctx = NULL;
  self->ssh = NULL;

  if (self->stop_request) {
    self->state = SFTP_STATE_CLOSED;
    goto done;
  }

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  char port_str[6];
  snprintf(port_str, sizeof(port_str), "%d", self->port);

  // See modssh.c's identical comment -- known residual gap, DNS can't be
  // bounded/interrupted the way connect() and the handshake wait below are.
  struct addrinfo *res = NULL;
  if (getaddrinfo(self->host, port_str, &hints, &res) != 0 || res == NULL) {
    self->last_error = SFTP_ERR_DNS;
    self->state = SFTP_STATE_FAILED;
    goto done;
  }

  self->sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (self->sockfd < 0) {
    freeaddrinfo(res);
    self->last_error = SFTP_ERR_SOCKET;
    self->state = SFTP_STATE_FAILED;
    goto done;
  }

  // Non-blocking connect(), bounded via sftp_wait_fd() -- see modssh.c's
  // identical block for the full rationale. Restored to blocking before
  // wolfSSH touches the fd.
  int sock_flags = fcntl(self->sockfd, F_GETFL, 0);
  fcntl(self->sockfd, F_SETFL, sock_flags | O_NONBLOCK);

  int connect_rc = connect(self->sockfd, res->ai_addr, res->ai_addrlen);
  freeaddrinfo(res);

  if (connect_rc != 0 && errno != EINPROGRESS) {
    self->last_error = SFTP_ERR_CONNECT;
    self->state = SFTP_STATE_FAILED;
    goto done;
  }

  if (connect_rc != 0) {
    int w = sftp_wait_fd(self, self->sockfd, true, SFTP_CONNECT_TIMEOUT_MS);
    if (w < 0) {
      self->last_error = SFTP_ERR_ABORTED;
      self->state = SFTP_STATE_CLOSED;
      goto done;
    }
    if (w == 0) {
      self->last_error = SFTP_ERR_CONNECT_TIMEOUT;
      self->state = SFTP_STATE_FAILED;
      goto done;
    }
    int so_err = 0;
    socklen_t so_err_len = sizeof(so_err);
    getsockopt(self->sockfd, SOL_SOCKET, SO_ERROR, &so_err, &so_err_len);
    if (so_err != 0) {
      self->last_error = SFTP_ERR_CONNECT;
      self->state = SFTP_STATE_FAILED;
      goto done;
    }
  }

  fcntl(self->sockfd, F_SETFL, sock_flags); // restore blocking

  // Best-effort: lets the OS/network stack notice a peer that vanishes
  // without a clean FIN/RST sooner than the OS's own TCP retransmission
  // timeout otherwise would (can be several minutes) -- see
  // sftp_process_op()'s comment on why a dead-but-silent connection
  // during a live operation can otherwise wedge the whole background
  // task (stop_request never gets checked again until that one blocking
  // wolfSSH_SFTP_* call finally gives up on its own). Plain SOL_SOCKET
  // option only, same as modsshd.c's identical use -- the IPPROTO_TCP-
  // level interval/count tuning knobs aren't verified against vendor
  // source for this ESP-IDF/lwIP build, so deliberately conservative
  // rather than precisely tuned.
  {
    int opt = 1;
    setsockopt(self->sockfd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
  }

  self->ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_CLIENT, NULL);
  if (self->ctx == NULL) {
    self->last_error = SFTP_ERR_CTX_NEW;
    self->state = SFTP_STATE_FAILED;
    goto done;
  }

  wolfSSH_SetUserAuth(self->ctx, sftp_user_auth);

  self->ssh = wolfSSH_new(self->ctx);
  if (self->ssh == NULL) {
    self->last_error = SFTP_ERR_SSH_NEW;
    self->state = SFTP_STATE_FAILED;
    goto done;
  }

  wolfSSH_SetUserAuthCtx(self->ssh, (void *)self);
  wolfSSH_set_fd(self->ssh, self->sockfd);
  wolfSSH_SetUsername(self->ssh, self->username);

  // wolfSSH_SFTP_connect() wraps wolfSSH_connect() (setting the
  // subsystem channel type itself), which has the same WS_WANT_READ-is-
  // fatal state machine as wolfSSH_accept() -- see modssh.c's identical
  // gate before its own wolfSSH_connect() call for the full rationale.
  {
    int w = sftp_wait_fd(self, self->sockfd, false, SFTP_HANDSHAKE_TIMEOUT_MS);
    if (w < 0) {
      self->last_error = SFTP_ERR_ABORTED;
      self->state = SFTP_STATE_CLOSED;
      goto done;
    }
    if (w == 0) {
      self->last_error = SFTP_ERR_HANDSHAKE_TIMEOUT;
      self->state = SFTP_STATE_FAILED;
      goto done;
    }
  }

  {
    int rc = wolfSSH_SFTP_connect(self->ssh);
    if (rc != WS_SUCCESS) {
      self->last_error = wolfSSH_get_error(self->ssh);
      self->state = SFTP_STATE_FAILED;
      goto done;
    }
  }

  self->state = SFTP_STATE_CONNECTED;

  while (!self->stop_request) {
    if (xSemaphoreTake(self->request_sem, pdMS_TO_TICKS(SFTP_REQUEST_POLL_MS)) !=
        pdTRUE) {
      continue;
    }
    sftp_process_op(self);
    xSemaphoreGive(self->response_sem);
  }

  self->state = SFTP_STATE_CLOSED;

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

// Runs the currently-filled-in request and blocks for the result. Every
// Client method funnels through this after filling in self->op/path_a/
// etc. Returns false (and sets last_error) if not connected or the op
// timed out -- callers should raise from that, not read stale op_ret.
static bool sftp_do_op(sftp_client_obj_t *self) {
  if (self->state != SFTP_STATE_CONNECTED) {
    self->last_error = SFTP_ERR_NOT_CONNECTED;
    return false;
  }
  xSemaphoreGive(self->request_sem);
  if (xSemaphoreTake(self->response_sem, pdMS_TO_TICKS(SFTP_OP_TIMEOUT_MS)) !=
      pdTRUE) {
    self->last_error = SFTP_ERR_OP_TIMEOUT;
    return false;
  }
  return true;
}

static void sftp_copy_path(char *dst, size_t dstSz, mp_obj_t src) {
  const char *s = mp_obj_str_get_str(src);
  strncpy(dst, s, dstSz - 1);
  dst[dstSz - 1] = '\0';
}

static mp_obj_t sftp_client_make_new(const mp_obj_type_t *type, size_t n_args,
                                     size_t n_kw, const mp_obj_t *args) {
  mp_arg_check_num(n_args, n_kw, 0, 0, false);

  sftp_client_obj_t *self = m_new_obj(sftp_client_obj_t);
  self->base.type = type;
  self->task = NULL;
  self->stack = NULL;
  self->state = SFTP_STATE_IDLE;
  self->stop_request = false;
  self->sockfd = -1;
  self->ctx = NULL;
  self->ssh = NULL;
  self->last_error = 0;

  self->request_sem = xSemaphoreCreateBinary();
  self->response_sem = xSemaphoreCreateBinary();
  if (self->request_sem == NULL || self->response_sem == NULL) {
    mp_raise_msg(&mp_type_MemoryError,
                MP_ERROR_TEXT("failed to allocate sftp semaphores"));
  }

  return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t sftp_client_connect(size_t n_args, const mp_obj_t *args) {
  sftp_client_obj_t *self = MP_OBJ_TO_PTR(args[0]);

  // self->task != NULL also guards here -- see modssh.c's ssh_client_connect()
  // for the full rationale (same race window between sftp_task() setting
  // state and it finishing its own cleanup/nulling self->task).
  if (self->state == SFTP_STATE_CONNECTING || self->state == SFTP_STATE_CONNECTED ||
      self->task != NULL) {
    mp_raise_msg(&mp_type_RuntimeError,
                MP_ERROR_TEXT("already connecting or connected"));
  }

  // Reclaim the previous run's PSRAM stack, if any -- see modssh.c's
  // identical reclaim for the full rationale.
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

  self->stop_request = false;
  self->last_error = 0;
  self->state = SFTP_STATE_CONNECTING;

  // PSRAM-backed stack -- see modssh.c's ssh_client_connect() for the
  // full rationale (same pattern, same 64KB-per-session internal-RAM
  // pressure this relieves).
  StackType_t *stack = heap_caps_malloc(SFTP_TASK_STACK_WORDS * sizeof(StackType_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (stack == NULL) {
    stack = heap_caps_malloc(SFTP_TASK_STACK_WORDS * sizeof(StackType_t),
                             MALLOC_CAP_8BIT);
  }
  if (stack == NULL) {
    self->state = SFTP_STATE_FAILED;
    mp_raise_msg(&mp_type_RuntimeError,
                MP_ERROR_TEXT("failed to allocate sftp task stack"));
  }
  self->stack = stack;

  TaskHandle_t task = xTaskCreateStaticPinnedToCore(
      sftp_task, "sftpclient", SFTP_TASK_STACK_WORDS, self, SFTP_TASK_PRIORITY,
      stack, &self->task_buf,
      1 /* APP CPU -- leave PRO CPU/core 0 for MicroPython */);

  if (task == NULL) {
    heap_caps_free(stack);
    self->stack = NULL;
    self->state = SFTP_STATE_FAILED;
    mp_raise_msg(&mp_type_RuntimeError,
                MP_ERROR_TEXT("failed to start sftp task"));
  }
  self->task = task;

  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sftp_client_connect_obj, 5, 5,
                                           sftp_client_connect);

static mp_obj_t sftp_client_status(mp_obj_t self_in) {
  sftp_client_obj_t *self = MP_OBJ_TO_PTR(self_in);
  return mp_obj_new_int(self->state);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sftp_client_status_obj, sftp_client_status);

static mp_obj_t sftp_client_error_code(mp_obj_t self_in) {
  sftp_client_obj_t *self = MP_OBJ_TO_PTR(self_in);
  return mp_obj_new_int(self->last_error);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sftp_client_error_code_obj, sftp_client_error_code);

static mp_obj_t sftp_client_disconnect(mp_obj_t self_in) {
  sftp_client_obj_t *self = MP_OBJ_TO_PTR(self_in);
  self->stop_request = true;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(sftp_client_disconnect_obj, sftp_client_disconnect);

// Converts a WS_SFTP_FILEATRB's permission word into a VFS-style mode
// int: high nibble carries the S_IFDIR/S_IFREG-equivalent bit (matches
// ftp.py's stat()/ilistdir() 0x4000/0x8000 convention) or'd with the low
// 9 permission bits.
static mp_int_t sftp_atrb_mode(const WS_SFTP_FILEATRB *atrb) {
  mp_int_t type_bit = ((atrb->per & 0170000) == 0040000) ? 0x4000 : 0x8000;
  return type_bit | (atrb->per & 0777);
}

static mp_obj_t sftp_atrb_tuple(const WS_SFTP_FILEATRB *atrb) {
  mp_obj_t items[3] = {
      mp_obj_new_int(sftp_atrb_mode(atrb)),
      mp_obj_new_int_from_uint(atrb->sz[0]), // practical file-size ceiling: 4GB
      mp_obj_new_int_from_uint(atrb->mtime), // word32, would go negative past 2038 via mp_obj_new_int
  };
  return mp_obj_new_tuple(3, items);
}

static mp_obj_t sftp_client_ls(mp_obj_t self_in, mp_obj_t path) {
  sftp_client_obj_t *self = MP_OBJ_TO_PTR(self_in);
  sftp_copy_path(self->path_a, sizeof(self->path_a), path);
  self->op = SFTP_OP_LS;

  if (!sftp_do_op(self) || self->op_ret != WS_SUCCESS) {
    mp_raise_OSError(MP_EIO);
  }

  mp_obj_t result = mp_obj_new_list(0, NULL);
  WS_SFTPNAME *n = self->ls_result;
  while (n != NULL) {
    if (strcmp(n->fName, ".") != 0 && strcmp(n->fName, "..") != 0) {
      mp_obj_t entry[2] = {
          mp_obj_new_str(n->fName, n->fSz),
          sftp_atrb_tuple(&n->atrb),
      };
      mp_obj_list_append(result, mp_obj_new_tuple(2, entry));
    }
    n = n->next;
  }
  wolfSSH_SFTPNAME_list_free(self->ls_result);
  self->ls_result = NULL;

  return result;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sftp_client_ls_obj, sftp_client_ls);

static mp_obj_t sftp_client_stat_impl(mp_obj_t self_in, mp_obj_t path, sftp_op_t op) {
  sftp_client_obj_t *self = MP_OBJ_TO_PTR(self_in);
  sftp_copy_path(self->path_a, sizeof(self->path_a), path);
  self->op = op;

  if (!sftp_do_op(self) || self->op_ret != WS_SUCCESS) {
    mp_raise_OSError(MP_EIO);
  }

  return sftp_atrb_tuple(&self->atrb);
}

static mp_obj_t sftp_client_stat(mp_obj_t self_in, mp_obj_t path) {
  return sftp_client_stat_impl(self_in, path, SFTP_OP_STAT);
}
static MP_DEFINE_CONST_FUN_OBJ_2(sftp_client_stat_obj, sftp_client_stat);

static mp_obj_t sftp_client_lstat(mp_obj_t self_in, mp_obj_t path) {
  return sftp_client_stat_impl(self_in, path, SFTP_OP_LSTAT);
}
static MP_DEFINE_CONST_FUN_OBJ_2(sftp_client_lstat_obj, sftp_client_lstat);

static mp_obj_t sftp_client_open(mp_obj_t self_in, mp_obj_t path, mp_obj_t flags) {
  sftp_client_obj_t *self = MP_OBJ_TO_PTR(self_in);
  sftp_copy_path(self->path_a, sizeof(self->path_a), path);
  self->open_reason = (word32)mp_obj_get_int(flags);
  self->op = SFTP_OP_OPEN;

  if (!sftp_do_op(self) || self->op_ret != WS_SUCCESS) {
    mp_raise_OSError(MP_EIO);
  }

  return mp_obj_new_bytes(self->handle, self->handle_sz);
}
static MP_DEFINE_CONST_FUN_OBJ_3(sftp_client_open_obj, sftp_client_open);

static mp_obj_t sftp_client_read(size_t n_args, const mp_obj_t *args) {
  sftp_client_obj_t *self = MP_OBJ_TO_PTR(args[0]);
  mp_buffer_info_t handle_buf;
  mp_get_buffer_raise(args[1], &handle_buf, MP_BUFFER_READ);
  if (handle_buf.len > sizeof(self->handle)) {
    mp_raise_ValueError(MP_ERROR_TEXT("handle too large"));
  }
  memcpy(self->handle, handle_buf.buf, handle_buf.len);
  self->handle_sz = (word32)handle_buf.len;

  mp_int_t offset = mp_obj_get_int(args[2]);
  self->ofst[0] = (word32)offset;
  self->ofst[1] = 0; // practical file-size ceiling: 4GB, see sftp_atrb_tuple()

  mp_int_t size = mp_obj_get_int(args[3]);
  if (size > (mp_int_t)sizeof(self->scratch)) {
    size = sizeof(self->scratch);
  }
  self->op = SFTP_OP_READ;

  if (!sftp_do_op(self) || self->op_ret != WS_SUCCESS) {
    mp_raise_OSError(MP_EIO);
  }

  return mp_obj_new_bytes(self->scratch, self->scratch_sz);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sftp_client_read_obj, 4, 4, sftp_client_read);

static mp_obj_t sftp_client_write(size_t n_args, const mp_obj_t *args) {
  sftp_client_obj_t *self = MP_OBJ_TO_PTR(args[0]);
  mp_buffer_info_t handle_buf;
  mp_get_buffer_raise(args[1], &handle_buf, MP_BUFFER_READ);
  if (handle_buf.len > sizeof(self->handle)) {
    mp_raise_ValueError(MP_ERROR_TEXT("handle too large"));
  }
  memcpy(self->handle, handle_buf.buf, handle_buf.len);
  self->handle_sz = (word32)handle_buf.len;

  mp_int_t offset = mp_obj_get_int(args[2]);
  self->ofst[0] = (word32)offset;
  self->ofst[1] = 0;

  mp_buffer_info_t data_buf;
  mp_get_buffer_raise(args[3], &data_buf, MP_BUFFER_READ);
  size_t n = data_buf.len < sizeof(self->scratch) ? data_buf.len : sizeof(self->scratch);
  memcpy(self->scratch, data_buf.buf, n);
  self->scratch_sz = (word32)n;
  self->op = SFTP_OP_WRITE;

  if (!sftp_do_op(self) || self->op_ret != WS_SUCCESS) {
    mp_raise_OSError(MP_EIO);
  }

  return mp_obj_new_int(self->scratch_sz);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sftp_client_write_obj, 4, 4, sftp_client_write);

static mp_obj_t sftp_client_close(mp_obj_t self_in, mp_obj_t handle) {
  sftp_client_obj_t *self = MP_OBJ_TO_PTR(self_in);
  mp_buffer_info_t handle_buf;
  mp_get_buffer_raise(handle, &handle_buf, MP_BUFFER_READ);
  if (handle_buf.len > sizeof(self->handle)) {
    mp_raise_ValueError(MP_ERROR_TEXT("handle too large"));
  }
  memcpy(self->handle, handle_buf.buf, handle_buf.len);
  self->handle_sz = (word32)handle_buf.len;
  self->op = SFTP_OP_CLOSE;

  if (!sftp_do_op(self) || self->op_ret != WS_SUCCESS) {
    mp_raise_OSError(MP_EIO);
  }
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sftp_client_close_obj, sftp_client_close);

static mp_obj_t sftp_client_path_op(mp_obj_t self_in, mp_obj_t path, sftp_op_t op) {
  sftp_client_obj_t *self = MP_OBJ_TO_PTR(self_in);
  sftp_copy_path(self->path_a, sizeof(self->path_a), path);
  self->op = op;

  if (!sftp_do_op(self) || self->op_ret != WS_SUCCESS) {
    mp_raise_OSError(MP_EIO);
  }
  return mp_const_none;
}

static mp_obj_t sftp_client_mkdir(mp_obj_t self_in, mp_obj_t path) {
  return sftp_client_path_op(self_in, path, SFTP_OP_MKDIR);
}
static MP_DEFINE_CONST_FUN_OBJ_2(sftp_client_mkdir_obj, sftp_client_mkdir);

static mp_obj_t sftp_client_rmdir(mp_obj_t self_in, mp_obj_t path) {
  return sftp_client_path_op(self_in, path, SFTP_OP_RMDIR);
}
static MP_DEFINE_CONST_FUN_OBJ_2(sftp_client_rmdir_obj, sftp_client_rmdir);

static mp_obj_t sftp_client_remove(mp_obj_t self_in, mp_obj_t path) {
  return sftp_client_path_op(self_in, path, SFTP_OP_REMOVE);
}
static MP_DEFINE_CONST_FUN_OBJ_2(sftp_client_remove_obj, sftp_client_remove);

static mp_obj_t sftp_client_rename(mp_obj_t self_in, mp_obj_t old, mp_obj_t new_) {
  sftp_client_obj_t *self = MP_OBJ_TO_PTR(self_in);
  sftp_copy_path(self->path_a, sizeof(self->path_a), old);
  sftp_copy_path(self->path_b, sizeof(self->path_b), new_);
  self->op = SFTP_OP_RENAME;

  if (!sftp_do_op(self) || self->op_ret != WS_SUCCESS) {
    mp_raise_OSError(MP_EIO);
  }
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(sftp_client_rename_obj, sftp_client_rename);

static const mp_rom_map_elem_t sftp_client_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&sftp_client_connect_obj)},
    {MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&sftp_client_status_obj)},
    {MP_ROM_QSTR(MP_QSTR_error_code), MP_ROM_PTR(&sftp_client_error_code_obj)},
    {MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&sftp_client_disconnect_obj)},
    {MP_ROM_QSTR(MP_QSTR_ls), MP_ROM_PTR(&sftp_client_ls_obj)},
    {MP_ROM_QSTR(MP_QSTR_stat), MP_ROM_PTR(&sftp_client_stat_obj)},
    {MP_ROM_QSTR(MP_QSTR_lstat), MP_ROM_PTR(&sftp_client_lstat_obj)},
    {MP_ROM_QSTR(MP_QSTR_open), MP_ROM_PTR(&sftp_client_open_obj)},
    {MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&sftp_client_read_obj)},
    {MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&sftp_client_write_obj)},
    {MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&sftp_client_close_obj)},
    {MP_ROM_QSTR(MP_QSTR_mkdir), MP_ROM_PTR(&sftp_client_mkdir_obj)},
    {MP_ROM_QSTR(MP_QSTR_rmdir), MP_ROM_PTR(&sftp_client_rmdir_obj)},
    {MP_ROM_QSTR(MP_QSTR_remove), MP_ROM_PTR(&sftp_client_remove_obj)},
    {MP_ROM_QSTR(MP_QSTR_rename), MP_ROM_PTR(&sftp_client_rename_obj)},
};
static MP_DEFINE_CONST_DICT(sftp_client_locals_dict, sftp_client_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(sftp_client_type, MP_QSTR_Client, MP_TYPE_FLAG_NONE,
                         make_new, sftp_client_make_new, locals_dict,
                         &sftp_client_locals_dict);

static const mp_rom_map_elem_t modsftp_module_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_modsftp)},
    {MP_ROM_QSTR(MP_QSTR_Client), MP_ROM_PTR(&sftp_client_type)},
    {MP_ROM_QSTR(MP_QSTR_IDLE), MP_ROM_INT(SFTP_STATE_IDLE)},
    {MP_ROM_QSTR(MP_QSTR_CONNECTING), MP_ROM_INT(SFTP_STATE_CONNECTING)},
    {MP_ROM_QSTR(MP_QSTR_CONNECTED), MP_ROM_INT(SFTP_STATE_CONNECTED)},
    {MP_ROM_QSTR(MP_QSTR_FAILED), MP_ROM_INT(SFTP_STATE_FAILED)},
    {MP_ROM_QSTR(MP_QSTR_CLOSED), MP_ROM_INT(SFTP_STATE_CLOSED)},
    {MP_ROM_QSTR(MP_QSTR_FXF_READ), MP_ROM_INT(WOLFSSH_FXF_READ)},
    {MP_ROM_QSTR(MP_QSTR_FXF_WRITE), MP_ROM_INT(WOLFSSH_FXF_WRITE)},
    {MP_ROM_QSTR(MP_QSTR_FXF_APPEND), MP_ROM_INT(WOLFSSH_FXF_APPEND)},
    {MP_ROM_QSTR(MP_QSTR_FXF_CREAT), MP_ROM_INT(WOLFSSH_FXF_CREAT)},
    {MP_ROM_QSTR(MP_QSTR_FXF_TRUNC), MP_ROM_INT(WOLFSSH_FXF_TRUNC)},
    {MP_ROM_QSTR(MP_QSTR_FXF_EXCL), MP_ROM_INT(WOLFSSH_FXF_EXCL)},
};
static MP_DEFINE_CONST_DICT(modsftp_module_globals, modsftp_module_globals_table);

const mp_obj_module_t modsftp_module = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&modsftp_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_modsftp, modsftp_module);
