/*
 * MicroPython SFTP server (wolfSSH-backed)
 * Copyright (c) 2026 8bitmcu
 * License: MIT
 *
 * wolfSSH drives the whole SFTP request/response cycle itself
 * (wolfSSH_SFTP_accept() then repeated wolfSSH_SFTP_read() calls, each
 * servicing one full request) -- we never touch the wire protocol
 * directly. What we provide is the actual file access: wolfSSH's
 * server-side SFTP handlers (src/wolfsftp.c) expect a
 * WOLFSSH_USER_FILESYSTEM (see myFilesystem.h), since this device's real
 * storage (boot.py's os.mount(partition, '/flash') with os.VfsLfs2) is a
 * MicroPython-VFS-level mount only, never registered with ESP-IDF's
 * esp_vfs/newlib syscall layer -- wolfSSH's generic-POSIX filesystem
 * handlers would compile but fail (ENOENT) against these paths.
 *
 * So every myFilesystem.h hook (open/read/write/close/stat/mkdir/...)
 * bridges from this background task to mp_task to run as a real Python
 * VFS call: mp_sched_schedule() (same mechanism as modsshd.c's connect/
 * disconnect notifier -- the only safe way for a background task to
 * reach into MicroPython) wakes mp_task, then a single-in-flight-request
 * handoff blocks this task until mp_task's scheduled callback (sftpd.py's
 * set_vfs_service() servicer, doing ordinary Python open()/read()/
 * write()/os.stat()/os.listdir() etc) reports back via request()/
 * respond(). wolfSSH_SFTP_read() processes one full request/response
 * cycle at a time even though real clients pipeline multiple requests
 * over the wire, so single-in-flight is sufficient -- no queue needed.
 *
 * Up to SFTPD_MAX_SESSIONS clients can be connected concurrently -- real
 * clients (FileZilla by default) open several simultaneous connections
 * per site. One listener task accept()s and spawns a dedicated FreeRTOS
 * task per connection (own PSRAM-backed stack, own WOLFSSH_CTX/WOLFSSH*,
 * own accept-handshake + steady-state loop -- see sftpd_session_task()/
 * sftpd_handle_client()); the VFS RPC bridge above stays a single shared
 * struct, serialized across sessions by vfs_mutex, since real embedded
 * flash I/O doesn't benefit from true parallelism anyway. sshd and sftpd
 * are independent, structurally different SSH subsystems/channel types,
 * with no shared session enforcement between them.
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
// NOT wolfssh/internal.h: it pulls in wolfcrypt's tfm.h, whose real
// mp_init(mp_int*) conflicts with MicroPython's own mp_init(void) from
// py/runtime.h.

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>

#include "myFilesystem.h"

#define SFTPD_TASK_STACK_WORDS 16384
#define SFTPD_TASK_PRIORITY 5
#define SFTPD_ACCEPT_POLL_MS 200
#define SFTPD_HANDSHAKE_TIMEOUT_MS 10000

// How long this task will wait for mp_task to service one VFS op --
// bounds a stuck request rather than hanging this task forever.
#define SFTPD_VFS_OP_TIMEOUT_MS 30000

#define SFTPD_PATH_MAX 256
// modsftpd_wpwrite()/wpread() loop internally over sub-chunks (see their
// own comments), so this can stay modest regardless of request size --
// a needlessly large buffer here eats into the same internal RAM wolfSSH
// needs for its own per-message allocations.
#define SFTPD_SCRATCH_SZ 4096
#define SFTPD_HOST_KEY_MAX_SZ 512

// Up to this many SFTP clients can be connected at once -- real clients
// (FileZilla by default) open multiple simultaneous connections per site.
// Each session's task stack (SFTPD_TASK_STACK_WORDS, ~64KB) is allocated
// from PSRAM rather than internal SRAM (see sftpd_task()'s accept loop),
// since 4x that in internal RAM would be a real squeeze on this board's
// ~512KB alongside sshd and everything else.
#define SFTPD_MAX_SESSIONS 4

typedef enum {
  SFTPD_STATE_IDLE = 0,
  SFTPD_STATE_LISTENING,
  SFTPD_STATE_STOPPED,
} sftpd_state_t;

// A session slot's lifecycle: FREE -> (listener accepts, spawns task) ->
// ACTIVE -> (session task's own work is done) -> FINISHED -> (listener's
// next poll reclaims the PSRAM stack buffer) -> FREE again. The reclaim
// step can't happen from inside the session task itself -- a task cannot
// free the stack memory it's currently executing on right before
// self-deleting. volatile, no mutex needed: each slot's state is only
// ever written by exactly one of {listener, that slot's own session
// task} at a time, same reasoning as stop_request below.
typedef enum {
  SFTPD_SLOT_FREE = 0,
  SFTPD_SLOT_ACTIVE,
  SFTPD_SLOT_FINISHED,
} sftpd_slot_state_t;

// Mirrors modsftp.c's client-side op enum, server direction. Values
// exposed to Python via request() so sftpd.py's servicer can dispatch.
typedef enum {
  SFTPD_OP_STAT = 1,
  SFTPD_OP_LSTAT,
  SFTPD_OP_OPEN,
  SFTPD_OP_READ,
  SFTPD_OP_WRITE,
  SFTPD_OP_CLOSE,
  SFTPD_OP_OPENDIR,
  SFTPD_OP_READDIR,
  SFTPD_OP_CLOSEDIR,
  SFTPD_OP_MKDIR,
  SFTPD_OP_RMDIR,
  SFTPD_OP_REMOVE,
  SFTPD_OP_RENAME,
  SFTPD_OP_GETCWD,
} sftpd_op_t;

typedef struct _sftpd_server_obj_t sftpd_server_obj_t;

// One slot per concurrent connection.
typedef struct {
  volatile sftpd_slot_state_t state;
  TaskHandle_t task;
  StaticTask_t task_buf;   // xTaskCreateStaticPinnedToCore()'s control block
  StackType_t *stack;      // PSRAM-backed, see sftpd_task()'s accept loop
  int client_fd;
  char client_ip[16];
  int last_error;
  // modsftpd_wreaddir()'s returned buffer, per-session rather than
  // shared: the caller (wolfSSH_SFTPNAME_readdir()) dereferences the
  // returned pointer after our function has already returned and
  // released vfs_mutex, so a shared buffer could be overwritten by
  // another session before the first caller reads it.
  struct dirent dirent_buf;
  sftpd_server_obj_t *server; // back-pointer to the shared server state below
} sftpd_session_t;

struct _sftpd_server_obj_t {
  mp_obj_base_t base;

  TaskHandle_t task; // the listener task
  StaticTask_t task_buf; // xTaskCreateStaticPinnedToCore()'s control block
                         // for the listener task -- distinct from each
                         // sftpd_session_t's own task_buf above
  StackType_t *stack;    // PSRAM-backed, see sftpd_server_start()
  volatile sftpd_state_t state;
  volatile bool stop_request;

  char username[32];
  char password[64];

  byte host_key[SFTPD_HOST_KEY_MAX_SZ];
  word32 host_key_sz;

  int port;
  int listen_fd;
  int last_error; // listener-level errors only (socket/bind/listen) -- see error_code()

  sftpd_session_t sessions[SFTPD_MAX_SESSIONS];

  mp_obj_t notify_cb;      // set_notify() -- same shape as modsshd.c's
  mp_obj_t vfs_service_cb; // set_vfs_service() -- the VFS bridge callback

  // Request/response RPC, shared across all sessions (see vfs_mutex's
  // comment at its point of use): single in-flight op at a time, so one
  // semaphore (mp_task's response) is enough -- vfs_mutex arbitrates
  // which session task gets to be the requester at any moment.
  SemaphoreHandle_t response_sem;
  SemaphoreHandle_t vfs_mutex;

  sftpd_op_t op;
  char path_a[SFTPD_PATH_MAX];
  char path_b[SFTPD_PATH_MAX]; // rename() target
  int flags;                   // WOLFSSH_O_* bitmask, OPEN only
  int handle;                  // in/out: WFD or WDIR value
  byte scratch[SFTPD_SCRATCH_SZ];
  word32 scratch_sz;           // in: bytes to write; out: bytes read
  word32 ofst;                 // practical file-size ceiling: 4GB, see modsftp.c's identical tradeoff
  word32 resp_mode, resp_size, resp_mtime; // STAT/LSTAT response (also: resp_size doubles as WRITE's byte count)
  char name[SFTPD_PATH_MAX];   // READDIR response
  bool eof;                    // READDIR response

  int op_ret; // 0 success, nonzero error -- set by respond()
};

// wolfssh/error.h's WS_* codes run continuously from -1 to at least
// -1097 -- these sentinels stay well clear of that span and of
// modssh.c's/modsshd.c's/modsftp.c's own -2000/-4000/-5000 ranges.
#define SFTPD_ERR_SOCKET -6000
#define SFTPD_ERR_BIND -6001
#define SFTPD_ERR_LISTEN -6002
#define SFTPD_ERR_CTX_NEW -6003
#define SFTPD_ERR_KEY -6004
#define SFTPD_ERR_SSH_NEW -6005
#define SFTPD_ERR_HANDSHAKE_TIMEOUT -6006
#define SFTPD_ERR_RETRY_EXHAUSTED -6007

// Safety valve for the "retry immediately, no select() wait" branches
// below (WS_CHAN_RXD/WS_REKEYING/WS_WINDOW_FULL): a tight C loop with no
// yield point starves this core's idle task and trips the ESP-IDF task
// watchdog, panicking the device. taskYIELD() every iteration plus a
// hard iteration cap turns a real stall into a clean, diagnosable
// failure instead of a device reset.
#define SFTPD_RETRY_MAX 500

const mp_obj_type_t sftpd_server_type;

// --- The VFS bridge: called from myFilesystem.h's macros, which run
// deep inside wolfSSH's own Recv* dispatch on this background task. ---

// Blocks until mp_task services the request already filled into self,
// or SFTPD_VFS_OP_TIMEOUT_MS elapses. Returns false (no allocation, no
// MicroPython state touched -- safe from this task) if no servicer is
// registered or the wait timed out; callers must treat that the same
// as an operation failure, not read self->op_ret (which wasn't set).
static bool sftpd_do_vfs_op(sftpd_server_obj_t *self) {
  if (self->vfs_service_cb == mp_const_none) {
    return false;
  }
  // mp_sched_schedule() returns false if MicroPython's fixed-size
  // scheduler queue is momentarily full -- the callback is silently
  // dropped, not queued. Retry until it's accepted or our own deadline
  // runs out. A real vTaskDelay(), not taskYIELD(): the queue only
  // drains once mp_task runs its scheduler dispatch, and
  // SFTPD_TASK_PRIORITY may be higher than mp_task's own priority --
  // taskYIELD() only reliably hands off to ready tasks at
  // equal-or-higher priority, so it could spin here without ever
  // actually giving a lower-priority mp_task the chance to drain it.
  TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SFTPD_VFS_OP_TIMEOUT_MS);
  while (!mp_sched_schedule(self->vfs_service_cb, MP_OBJ_FROM_PTR(self))) {
    if (xTaskGetTickCount() >= deadline) {
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  TickType_t now = xTaskGetTickCount();
  TickType_t remaining = (now >= deadline) ? 0 : (deadline - now);
  if (xSemaphoreTake(self->response_sem, remaining) != pdTRUE) {
    return false;
  }
  return true;
}

static void sftpd_set_path_a(sftpd_server_obj_t *self, const char *path) {
  strncpy(self->path_a, path, sizeof(self->path_a) - 1);
  self->path_a[sizeof(self->path_a) - 1] = '\0';
}

// Every hook below receives `fs` as a sftpd_session_t* (set via
// wolfSSH_SetFilesystemHandle(ssh, session) in sftpd_handle_client()),
// not the shared sftpd_server_obj_t directly -- needed so
// modsftpd_wreaddir() has somewhere session-local to stash its returned
// dirent. Every hook takes vfs_mutex for its whole body (single exit
// point) since the shared RPC struct is visited by up to
// SFTPD_MAX_SESSIONS concurrent session tasks.

int modsftpd_wopen(void *fs, const char *path, int flags, unsigned int mode) {
  (void)mode;
  sftpd_session_t *session = (sftpd_session_t *)fs;
  sftpd_server_obj_t *self = session->server;
  xSemaphoreTake(self->vfs_mutex, portMAX_DELAY);
  sftpd_set_path_a(self, path);
  self->flags = flags;
  self->op = SFTPD_OP_OPEN;
  int ret = (!sftpd_do_vfs_op(self) || self->op_ret != 0) ? -1 : self->handle;
  xSemaphoreGive(self->vfs_mutex);
  return ret;
}

int modsftpd_wpread(void *fs, WFD fd, unsigned char *buf, unsigned int sz,
                    const unsigned int *ofst) {
  sftpd_session_t *session = (sftpd_session_t *)fs;
  sftpd_server_obj_t *self = session->server;
  xSemaphoreTake(self->vfs_mutex, portMAX_DELAY);
  // A single READ request can ask for more than one scratch-buffer's
  // worth, and some real clients expect exactly the requested amount
  // back (short of genuine EOF) -- loop accumulating VFS reads until
  // either sz bytes are collected or the file runs out.
  unsigned int done = 0;
  bool failed = false;
  while (done < sz && !failed) {
    // self->handle must be (re)set every iteration: sftpd_server_respond()
    // (inside sftpd_do_vfs_op()) unconditionally overwrites self->handle
    // with whatever resp_handle Python sent back, which defaults to -1
    // for anything that isn't OPEN/OPENDIR.
    self->handle = fd;
    self->ofst = ofst[0] + done;
    self->op = SFTPD_OP_READ;
    if (!sftpd_do_vfs_op(self) || self->op_ret != 0) {
      failed = true;
      break;
    }
    word32 n = self->scratch_sz;
    word32 remaining = sz - done;
    if (n > remaining) {
      n = remaining;
    }
    memcpy(buf + done, self->scratch, n);
    done += n;
    if (self->scratch_sz == 0) {
      break; // genuine EOF -- nothing more to read, not an error
    }
  }
  int ret = failed ? -1 : (int)done;
  xSemaphoreGive(self->vfs_mutex);
  return ret;
}

int modsftpd_wpwrite(void *fs, WFD fd, unsigned char *buf, unsigned int sz,
                     const unsigned int *ofst) {
  sftpd_session_t *session = (sftpd_session_t *)fs;
  sftpd_server_obj_t *self = session->server;
  xSemaphoreTake(self->vfs_mutex, portMAX_DELAY);
  // Loops over buf/sz in chunks of at most sizeof(self->scratch) -- a
  // single WRITE request can be larger than any one fixed buffer size a
  // real client sends. Must never report success for less than the full
  // requested size: wolfSSH_SFTP_RecvWrite() (the only caller) treats
  // any non-negative return as a complete write regardless of whether
  // it actually matches sz.
  unsigned int done = 0;
  bool failed = false;
  while (done < sz && !failed) {
    // self->handle must be (re)set every iteration -- see
    // modsftpd_wpread()'s identical comment just above for why.
    self->handle = fd;
    word32 chunk = (sz - done < sizeof(self->scratch))
                       ? (sz - done)
                       : sizeof(self->scratch);
    memcpy(self->scratch, buf + done, chunk);
    self->scratch_sz = chunk;
    self->ofst = ofst[0] + done;
    self->op = SFTPD_OP_WRITE;
    if (!sftpd_do_vfs_op(self) || self->op_ret != 0) {
      failed = true;
      break;
    }
    done += self->resp_size; // respond()'s "size" doubles as bytes-written here
    if (self->resp_size != chunk) {
      // Short write (disk full, etc) -- whatever's in `done` so far is
      // genuinely committed, but the overall request still isn't fully
      // satisfied.
      failed = true;
    }
  }
  int ret = (failed || done != sz) ? -1 : (int)done;
  xSemaphoreGive(self->vfs_mutex);
  return ret;
}

int modsftpd_wclose(void *fs, WFD fd) {
  sftpd_session_t *session = (sftpd_session_t *)fs;
  sftpd_server_obj_t *self = session->server;
  xSemaphoreTake(self->vfs_mutex, portMAX_DELAY);
  self->handle = fd;
  self->op = SFTPD_OP_CLOSE;
  int ret = (!sftpd_do_vfs_op(self) || self->op_ret != 0) ? -1 : 0;
  xSemaphoreGive(self->vfs_mutex);
  return ret;
}

int modsftpd_wopendir(void *fs, void *heap, WDIR *ctx, const char *path) {
  (void)heap;
  sftpd_session_t *session = (sftpd_session_t *)fs;
  sftpd_server_obj_t *self = session->server;
  xSemaphoreTake(self->vfs_mutex, portMAX_DELAY);
  sftpd_set_path_a(self, path);
  self->op = SFTPD_OP_OPENDIR;
  int ret;
  if (!sftpd_do_vfs_op(self) || self->op_ret != 0) {
    ret = -1; // WOPENDIR's caller (RecvOpenDir) checks `!= 0` as error
  } else {
    *ctx = self->handle;
    ret = 0;
  }
  xSemaphoreGive(self->vfs_mutex);
  return ret;
}

struct dirent *modsftpd_wreaddir(void *fs, WDIR *ctx) {
  sftpd_session_t *session = (sftpd_session_t *)fs;
  sftpd_server_obj_t *self = session->server;
  xSemaphoreTake(self->vfs_mutex, portMAX_DELAY);
  self->handle = *ctx;
  self->op = SFTPD_OP_READDIR;
  struct dirent *ret;
  if (!sftpd_do_vfs_op(self) || self->op_ret != 0 || self->eof) {
    ret = NULL;
  } else {
    // The only caller (wolfSSH_SFTPNAME_readdir()'s generic
    // implementation) only ever reads d_name.
    strncpy(session->dirent_buf.d_name, self->name,
            sizeof(session->dirent_buf.d_name) - 1);
    session->dirent_buf.d_name[sizeof(session->dirent_buf.d_name) - 1] = '\0';
    ret = &session->dirent_buf;
  }
  xSemaphoreGive(self->vfs_mutex);
  return ret;
}

int modsftpd_wclosedir(void *fs, WDIR *ctx) {
  sftpd_session_t *session = (sftpd_session_t *)fs;
  sftpd_server_obj_t *self = session->server;
  xSemaphoreTake(self->vfs_mutex, portMAX_DELAY);
  self->handle = *ctx;
  self->op = SFTPD_OP_CLOSEDIR;
  sftpd_do_vfs_op(self); // best-effort -- callers don't check this return value
  xSemaphoreGive(self->vfs_mutex);
  return 0;
}

int modsftpd_wmkdir(void *fs, const char *path, unsigned int mode) {
  (void)mode;
  sftpd_session_t *session = (sftpd_session_t *)fs;
  sftpd_server_obj_t *self = session->server;
  xSemaphoreTake(self->vfs_mutex, portMAX_DELAY);
  sftpd_set_path_a(self, path);
  self->op = SFTPD_OP_MKDIR;
  int ret = (!sftpd_do_vfs_op(self) || self->op_ret != 0) ? -1 : 0;
  xSemaphoreGive(self->vfs_mutex);
  return ret;
}

int modsftpd_wrmdir(void *fs, const char *path) {
  sftpd_session_t *session = (sftpd_session_t *)fs;
  sftpd_server_obj_t *self = session->server;
  xSemaphoreTake(self->vfs_mutex, portMAX_DELAY);
  sftpd_set_path_a(self, path);
  self->op = SFTPD_OP_RMDIR;
  int ret = (!sftpd_do_vfs_op(self) || self->op_ret != 0) ? -1 : 0;
  xSemaphoreGive(self->vfs_mutex);
  return ret;
}

int modsftpd_wremove(void *fs, const char *path) {
  sftpd_session_t *session = (sftpd_session_t *)fs;
  sftpd_server_obj_t *self = session->server;
  xSemaphoreTake(self->vfs_mutex, portMAX_DELAY);
  sftpd_set_path_a(self, path);
  self->op = SFTPD_OP_REMOVE;
  int ret = (!sftpd_do_vfs_op(self) || self->op_ret != 0) ? -1 : 0;
  xSemaphoreGive(self->vfs_mutex);
  return ret;
}

int modsftpd_wrename(void *fs, const char *oldPath, const char *newPath) {
  sftpd_session_t *session = (sftpd_session_t *)fs;
  sftpd_server_obj_t *self = session->server;
  xSemaphoreTake(self->vfs_mutex, portMAX_DELAY);
  sftpd_set_path_a(self, oldPath);
  strncpy(self->path_b, newPath, sizeof(self->path_b) - 1);
  self->path_b[sizeof(self->path_b) - 1] = '\0';
  self->op = SFTPD_OP_RENAME;
  int ret = (!sftpd_do_vfs_op(self) || self->op_ret != 0) ? -1 : 0;
  xSemaphoreGive(self->vfs_mutex);
  return ret;
}

// Called once per session by wolfSSH_SFTP_RecvRealPath() to bootstrap
// ssh->sftpDefaultPath the first time a client resolves "." or its
// initial directory (cached by wolfSSH itself after the first call).
// Wired to a real os.getcwd() call rather than a hardcoded path so it
// reflects wherever the physical shell actually is.
char *modsftpd_wgetcwd(void *fs, char *buf, unsigned int sz) {
  sftpd_session_t *session = (sftpd_session_t *)fs;
  sftpd_server_obj_t *self = session->server;
  xSemaphoreTake(self->vfs_mutex, portMAX_DELAY);
  self->op = SFTPD_OP_GETCWD;
  char *ret;
  if (!sftpd_do_vfs_op(self) || self->op_ret != 0) {
    ret = NULL; // matches real getcwd()'s NULL-on-error convention
  } else {
    strncpy(buf, self->name, sz);
    buf[sz - 1] = '\0';
    ret = buf;
  }
  xSemaphoreGive(self->vfs_mutex);
  return ret;
}

// Populates a WS_SFTP_FILEATRB from the last respond() call's mode/
// size/mtime. uid/gid always 0 (no real users on this device), atime
// mirrors mtime. Called with vfs_mutex already held by the caller.
static void sftpd_fill_atrb(sftpd_server_obj_t *self, WS_SFTP_FILEATRB *atr) {
  memset(atr, 0, sizeof(*atr));
  atr->flags = WOLFSSH_FILEATRB_SIZE | WOLFSSH_FILEATRB_UIDGID |
               WOLFSSH_FILEATRB_PERM | WOLFSSH_FILEATRB_TIME;
  atr->sz[0] = self->resp_size;
  atr->sz[1] = 0;
  atr->uid = 0;
  atr->gid = 0;
  atr->per = self->resp_mode;
  atr->atime = self->resp_mtime;
  atr->mtime = self->resp_mtime;
}

// wolfsftp.c's own forward declaration for these is `#ifndef
// WOLFSSH_USER_FILESYSTEM` -- skipped for us, so it expects an
// externally-linked definition rather than a static one.
int SFTP_GetAttributes(void *fs, const char *fileName, WS_SFTP_FILEATRB *atr,
                       unsigned char noFollow, void *heap) {
  (void)heap;
  sftpd_session_t *session = (sftpd_session_t *)fs;
  sftpd_server_obj_t *self = session->server;
  xSemaphoreTake(self->vfs_mutex, portMAX_DELAY);
  sftpd_set_path_a(self, fileName);
  self->op = noFollow ? SFTPD_OP_LSTAT : SFTPD_OP_STAT;
  int ret;
  if (!sftpd_do_vfs_op(self) || self->op_ret != 0) {
    ret = WS_BAD_FILE_E;
  } else {
    sftpd_fill_atrb(self, atr);
    ret = WS_SUCCESS;
  }
  xSemaphoreGive(self->vfs_mutex);
  return ret;
}

int SFTP_GetAttributes_Handle(void *sshVoid, unsigned char *handle,
                              int handleSz, WS_SFTP_FILEATRB *atr) {
  (void)handleSz;
  WOLFSSH *ssh = (WOLFSSH *)sshVoid;
  sftpd_session_t *session =
      (sftpd_session_t *)wolfSSH_GetFilesystemHandle(ssh);
  sftpd_server_obj_t *self = session->server;
  xSemaphoreTake(self->vfs_mutex, portMAX_DELAY);
  WFD fd;
  memcpy(&fd, handle, sizeof(fd));
  self->handle = fd;
  self->op = SFTPD_OP_STAT; // no real fstat -- sftpd.py re-stats the path it opened this handle against
  int ret;
  if (!sftpd_do_vfs_op(self) || self->op_ret != 0) {
    ret = WS_BAD_FILE_E;
  } else {
    sftpd_fill_atrb(self, atr);
    ret = WS_SUCCESS;
  }
  xSemaphoreGive(self->vfs_mutex);
  return ret;
}

// --- Connection lifecycle: mirrors modsshd.c's sshd_task()/
// sshd_handle_client() shape directly. ---

static int sftpd_user_auth(byte authType, WS_UserAuthData *authData, void *ctx) {
  sftpd_server_obj_t *self = (sftpd_server_obj_t *)ctx;

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

static void sftpd_handle_client(sftpd_session_t *session) {
  sftpd_server_obj_t *self = session->server;
  int slot = (int)(session - self->sessions);
  WOLFSSH_CTX *ctx = NULL;
  WOLFSSH *ssh = NULL;
  bool notified_connect = false;

  session->last_error = 0;

  ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, NULL);
  if (ctx == NULL) {
    session->last_error = SFTPD_ERR_CTX_NEW;
    goto done;
  }

  if (wolfSSH_CTX_UsePrivateKey_buffer(ctx, self->host_key, self->host_key_sz,
                                       WOLFSSH_FORMAT_ASN1) != WS_SUCCESS) {
    session->last_error = SFTPD_ERR_KEY;
    goto done;
  }

  wolfSSH_SetUserAuth(ctx, sftpd_user_auth);

  ssh = wolfSSH_new(ctx);
  if (ssh == NULL) {
    session->last_error = SFTPD_ERR_SSH_NEW;
    goto done;
  }

  wolfSSH_SetUserAuthCtx(ssh, (void *)self);
  wolfSSH_SetFilesystemHandle(ssh, (void *)session);
  wolfSSH_set_fd(ssh, session->client_fd);

  // wolfSSH_SFTP_accept() is its own resumable state machine, same shape
  // as wolfSSH_SFTP_read() in the steady-state loop below: a
  // non-WS_SUCCESS return isn't necessarily fatal, and
  // WS_CHAN_RXD/WS_REKEYING/WS_WINDOW_FULL mean retry immediately (data
  // already buffered internally), not wait on select(). Bounded overall
  // by SFTPD_HANDSHAKE_TIMEOUT_MS so a peer that never responds doesn't
  // hang this task forever.
  {
    TickType_t deadline = xTaskGetTickCount() +
                           pdMS_TO_TICKS(SFTPD_HANDSHAKE_TIMEOUT_MS);
    int accept_ret;
    int attempt = 0;
    for (;;) {
      attempt++;
      accept_ret = wolfSSH_SFTP_accept(ssh);
      int err = wolfSSH_get_error(ssh);
      // wolfSSH_SFTP_accept()'s documented success return is
      // WS_SFTP_COMPLETE (-1055), not WS_SUCCESS.
      if (accept_ret == WS_SUCCESS || accept_ret == WS_SFTP_COMPLETE) {
        break;
      }
      if (accept_ret == WS_REKEYING) {
        // wolfSSH_SFTP_accept()'s own internal dispatch can return this
        // directly, not just via wolfSSH_get_error().
        if (xTaskGetTickCount() >= deadline || attempt >= SFTPD_RETRY_MAX) {
          session->last_error = xTaskGetTickCount() >= deadline
                                     ? SFTPD_ERR_HANDSHAKE_TIMEOUT
                                     : SFTPD_ERR_RETRY_EXHAUSTED;
          goto done;
        }
        taskYIELD();
        continue;
      }
      if (accept_ret != WS_FATAL_ERROR) {
        // A specific, meaningful failure code returned directly -- trust
        // it over wolfSSH_get_error(), which can be a stale WS_CHAN_RXD
        // left over from an earlier, already-resolved event.
        session->last_error = accept_ret;
        goto done;
      }
      if (err == WS_CHAN_RXD || err == WS_REKEYING ||
          err == WS_WINDOW_FULL) {
        if (xTaskGetTickCount() >= deadline || attempt >= SFTPD_RETRY_MAX) {
          session->last_error = xTaskGetTickCount() >= deadline
                                     ? SFTPD_ERR_HANDSHAKE_TIMEOUT
                                     : SFTPD_ERR_RETRY_EXHAUSTED;
          goto done;
        }
        taskYIELD(); // see SFTPD_RETRY_MAX's comment -- must not busy-spin
        continue; // already-buffered data pending -- retry now, no wait
      }
      if (err == WS_WANT_READ || err == WS_WANT_WRITE) {
        // Select on whichever direction is actually needed -- same fix
        // as the steady-state loop below.
        fd_set hfds;
        FD_ZERO(&hfds);
        FD_SET(session->client_fd, &hfds);
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) {
          session->last_error = SFTPD_ERR_HANDSHAKE_TIMEOUT;
          goto done;
        }
        long remaining_ms = (long)(deadline - now) * portTICK_PERIOD_MS;
        struct timeval htv;
        htv.tv_sec = remaining_ms / 1000;
        htv.tv_usec = (remaining_ms % 1000) * 1000;
        fd_set *rset = (err == WS_WANT_READ) ? &hfds : NULL;
        fd_set *wset = (err == WS_WANT_WRITE) ? &hfds : NULL;
        if (select(session->client_fd + 1, rset, wset, NULL, &htv) <= 0) {
          session->last_error = SFTPD_ERR_HANDSHAKE_TIMEOUT;
          goto done;
        }
        continue;
      }
      // wolfSSH_get_error() doesn't always reflect a genuine failure --
      // fall back to the call's own return value rather than store a
      // misleadingly-clean 0.
      session->last_error = (err != 0) ? err : accept_ret;
      goto done;
    }
  }

  // notify_cb(code): code = slot+1 if connected, -(slot+1) if
  // disconnected -- a single allocation-free small int encoding both
  // which session and which event, since mp_sched_schedule() takes
  // exactly one mp_obj_t and this runs on a background task where
  // allocating a tuple/string is unsafe. sftpd.py's _on_event() decodes
  // it and calls session_ip(slot) (safe to allocate a str there) for the
  // status-bar message.
  if (self->notify_cb != mp_const_none) {
    mp_sched_schedule(self->notify_cb, MP_OBJ_NEW_SMALL_INT(slot + 1));
  }
  notified_connect = true;

  // Steady state: wolfSSH_SFTP_read() handles one full request/response
  // cycle per call, dispatching internally to the Recv* handlers, which
  // call our myFilesystem.h hooks.
  //
  // A non-WS_SUCCESS return does NOT necessarily mean the connection
  // died, and does NOT always mean "wait for more socket bytes" either:
  // WS_CHAN_RXD means a full SSH_MSG_CHANNEL_DATA packet was already
  // parsed off the wire and buffered into the channel, so the data
  // wolfSSH_SFTP_read() needs is already sitting in memory -- it must be
  // called again immediately, not routed back through select(), since no
  // new bytes are coming. Only WS_WANT_READ/WS_WANT_WRITE are genuine
  // "wait for the socket" signals.
  //
  // need_write persists across outer-loop iterations (not reset until
  // the next select() actually runs): this loop's select() must watch
  // for write-readiness whenever the previous pass ended on
  // WS_WANT_WRITE (the socket's send buffer was full, flushing a queued
  // response) -- watching the wrong direction means we'd wait for the
  // client to send something while the client is waiting for our
  // response, and neither side ever proceeds until the client's own
  // inactivity timeout gives up.
  bool need_write = false;
  while (!self->stop_request) {
    fd_set rfds, wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    if (need_write) {
      FD_SET(session->client_fd, &wfds);
    } else {
      FD_SET(session->client_fd, &rfds);
    }
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = SFTPD_ACCEPT_POLL_MS * 1000;
    int sel = select(session->client_fd + 1, &rfds, &wfds, NULL, &tv);
    if (sel <= 0 || !FD_ISSET(session->client_fd,
                              need_write ? &wfds : &rfds)) {
      continue;
    }
    need_write = false;

    int retry_count = 0; // see SFTPD_RETRY_MAX's comment
    for (;;) {
      int read_ret = wolfSSH_SFTP_read(ssh);
      int err = wolfSSH_get_error(ssh);
      int peek = wolfSSH_stream_peek(ssh, NULL, 1);
      if (read_ret == WS_SUCCESS) {
        // A real client can pipeline several SFTP requests back to back
        // in the same TCP segment (e.g. WRITE immediately followed by
        // CLOSE) -- wolfSSH's stream layer may have already consumed
        // all of that into its internal channel buffer during THIS
        // read, even though only one full request/response cycle was
        // dispatched. Going straight back to select() at that point
        // waits for genuinely new socket bytes that will never come.
        // wolfSSH_stream_peek(ssh, NULL, 1) (buf=NULL is safe) reports
        // bytes already sitting in the channel buffer without touching
        // the socket, so use it to decide whether to retry immediately.
        if (peek > 0) {
          // Each iteration here does real work (a full VFS RPC round
          // trip), so this isn't a tight no-progress spin and doesn't
          // need SFTPD_RETRY_MAX's hard cap -- a large upload can
          // legitimately pipeline hundreds of WRITE messages back to
          // back. Still yields to avoid starving mp_task (which this
          // task's own VFS RPC depends on) and other tasks on this core.
          taskYIELD();
          continue;
        }
        break; // this request/response cycle is done -- back to select()
      }
      if (read_ret == WS_REKEYING) {
        // wolfSSH_SFTP_read()'s STATE_RECV_SEND case can return this
        // directly, not just via wolfSSH_get_error().
        if (++retry_count >= SFTPD_RETRY_MAX) {
          session->last_error = SFTPD_ERR_RETRY_EXHAUSTED;
          goto done;
        }
        taskYIELD();
        continue;
      }
      if (read_ret != WS_FATAL_ERROR) {
        // A specific, meaningful failure code returned directly (e.g.
        // WS_MEMORY_E) -- trust it over wolfSSH_get_error(): ssh->error
        // can be a stale WS_CHAN_RXD left over from an earlier,
        // already-resolved event (wolfSSH_stream_read()'s own internal
        // retry loop resolves WS_CHAN_RXD locally but never resets the
        // persistent ssh->error field the way it does for
        // WS_WANT_READ/WS_WANT_WRITE).
        session->last_error = read_ret;
        goto done;
      }
      if (err == WS_CHAN_RXD || err == WS_REKEYING ||
          err == WS_WINDOW_FULL) {
        if (++retry_count >= SFTPD_RETRY_MAX) {
          session->last_error = SFTPD_ERR_RETRY_EXHAUSTED;
          goto done;
        }
        taskYIELD(); // must not busy-spin -- see SFTPD_RETRY_MAX's comment
        continue; // already-buffered data pending -- retry now, no wait
      }
      if (err == WS_WANT_READ) {
        break; // genuinely need the socket to become readable again
      }
      if (err == WS_WANT_WRITE) {
        need_write = true;
        break; // genuinely need the socket to become writable again
      }
      // wolfSSH_get_error() doesn't always reflect a genuine failure --
      // fall back to the read's own return value when err isn't
      // informative (can be a misleadingly-clean 0 despite a real,
      // detected failure).
      session->last_error = (err != 0) ? err : read_ret;
      goto done;
    }
  }

done:
  if (notified_connect && self->notify_cb != mp_const_none) {
    mp_sched_schedule(self->notify_cb, MP_OBJ_NEW_SMALL_INT(-(slot + 1)));
  }
  if (ssh) {
    wolfSSH_shutdown(ssh);
    wolfSSH_free(ssh);
  }
  if (ctx) {
    wolfSSH_CTX_free(ctx);
  }
}

// A session's whole lifetime: runs sftpd_handle_client() (blocks for the
// whole connection), then closes its socket, marks its own slot
// SFTPD_SLOT_FINISHED (not SFTPD_SLOT_FREE -- this task cannot free its
// own PSRAM stack buffer), and self-deletes. The listener's own poll
// loop reclaims the slot next.
static void sftpd_session_task(void *arg) {
  sftpd_session_t *session = (sftpd_session_t *)arg;

  sftpd_handle_client(session);

  close(session->client_fd);
  session->client_fd = -1;
  session->state = SFTPD_SLOT_FINISHED;
  vTaskDelete(NULL);
}

static void sftpd_task(void *arg) {
  sftpd_server_obj_t *self = (sftpd_server_obj_t *)arg;

  static bool wolfssh_lib_initialized = false;
  if (!wolfssh_lib_initialized) {
    wolfSSH_Init();
    wolfssh_lib_initialized = true;
  }

  self->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (self->listen_fd < 0) {
    self->last_error = SFTPD_ERR_SOCKET;
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
      self->last_error = SFTPD_ERR_BIND;
      goto done;
    }
  }

  // Backlog covers a few more pending connections than active session
  // slots -- once all slots are busy, incoming connections just queue
  // here rather than being abruptly refused.
  if (listen(self->listen_fd, SFTPD_MAX_SESSIONS + 2) != 0) {
    self->last_error = SFTPD_ERR_LISTEN;
    goto done;
  }

  self->state = SFTPD_STATE_LISTENING;

  while (!self->stop_request) {
    // Reclaim any slots whose session task already self-deleted -- see
    // sftpd_slot_state_t's comment for why this can't happen from
    // inside the session task itself.
    for (int i = 0; i < SFTPD_MAX_SESSIONS; i++) {
      if (self->sessions[i].state == SFTPD_SLOT_FINISHED) {
        heap_caps_free(self->sessions[i].stack);
        self->sessions[i].stack = NULL;
        self->sessions[i].task = NULL;
        self->sessions[i].state = SFTPD_SLOT_FREE;
      }
    }

    fd_set afds;
    FD_ZERO(&afds);
    FD_SET(self->listen_fd, &afds);
    struct timeval atv;
    atv.tv_sec = 0;
    atv.tv_usec = SFTPD_ACCEPT_POLL_MS * 1000;
    int asel = select(self->listen_fd + 1, &afds, NULL, NULL, &atv);
    if (asel <= 0 || !FD_ISSET(self->listen_fd, &afds)) {
      continue;
    }

    // Find a free slot before accept()ing -- if none is free, leave the
    // pending connection in the kernel's listen backlog.
    int slot = -1;
    for (int i = 0; i < SFTPD_MAX_SESSIONS; i++) {
      if (self->sessions[i].state == SFTPD_SLOT_FREE) {
        slot = i;
        break;
      }
    }
    if (slot < 0) {
      continue;
    }

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int fd = accept(self->listen_fd, (struct sockaddr *)&client_addr,
                    &client_len);
    if (fd < 0) {
      continue;
    }

    // Stack allocated from PSRAM, falling back to internal RAM if
    // PSRAM's exhausted -- see SFTPD_MAX_SESSIONS's comment.
    StackType_t *stack = heap_caps_malloc(
        SFTPD_TASK_STACK_WORDS * sizeof(StackType_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (stack == NULL) {
      stack = heap_caps_malloc(SFTPD_TASK_STACK_WORDS * sizeof(StackType_t),
                                MALLOC_CAP_8BIT);
    }
    if (stack == NULL) {
      // Both PSRAM and internal RAM exhausted -- reject this connection
      // cleanly rather than crash; the slot stays SFTPD_SLOT_FREE.
      close(fd);
      continue;
    }

    sftpd_session_t *session = &self->sessions[slot];
    session->stack = stack;
    session->client_fd = fd;
    session->last_error = 0;
    session->server = self;
    inet_ntop(AF_INET, &client_addr.sin_addr, session->client_ip,
             sizeof(session->client_ip));

    {
      int opt = 1;
      setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
    }

    TaskHandle_t task = xTaskCreateStaticPinnedToCore(
        sftpd_session_task, "sftpd-session", SFTPD_TASK_STACK_WORDS, session,
        SFTPD_TASK_PRIORITY, stack, &session->task_buf,
        1 /* APP CPU -- leave PRO CPU/core 0 for MicroPython */);
    if (task == NULL) {
      heap_caps_free(stack);
      session->stack = NULL;
      close(fd);
      session->client_fd = -1;
      continue; // slot never marked ACTIVE, stays FREE
    }
    session->task = task;
    session->state = SFTPD_SLOT_ACTIVE;
  }

done:
  self->state = SFTPD_STATE_STOPPED;
  if (self->listen_fd >= 0) {
    close(self->listen_fd);
    self->listen_fd = -1;
  }
  self->task = NULL;
  vTaskDelete(NULL);
}

// --- Constructor: sftpd.Server(username, password, host_key, port) ---

static mp_obj_t sftpd_server_make_new(const mp_obj_type_t *type, size_t n_args,
                                      size_t n_kw, const mp_obj_t *args) {
  mp_arg_check_num(n_args, n_kw, 4, 4, false);

  sftpd_server_obj_t *self = m_new_obj(sftpd_server_obj_t);
  self->base.type = type;

  self->notify_cb = mp_const_none;
  self->vfs_service_cb = mp_const_none;

  const char *username = mp_obj_str_get_str(args[0]);
  const char *password = mp_obj_str_get_str(args[1]);
  strncpy(self->username, username, sizeof(self->username) - 1);
  self->username[sizeof(self->username) - 1] = '\0';
  strncpy(self->password, password, sizeof(self->password) - 1);
  self->password[sizeof(self->password) - 1] = '\0';

  mp_buffer_info_t key_buf;
  mp_get_buffer_raise(args[2], &key_buf, MP_BUFFER_READ);
  if (key_buf.len > SFTPD_HOST_KEY_MAX_SZ) {
    mp_raise_ValueError(MP_ERROR_TEXT("host key too large"));
  }
  memcpy(self->host_key, key_buf.buf, key_buf.len);
  self->host_key_sz = (word32)key_buf.len;

  self->port = mp_obj_get_int(args[3]);

  self->task = NULL;
  self->stack = NULL;
  self->state = SFTPD_STATE_IDLE;
  self->stop_request = false;
  self->listen_fd = -1;
  self->last_error = 0;

  for (int i = 0; i < SFTPD_MAX_SESSIONS; i++) {
    self->sessions[i].state = SFTPD_SLOT_FREE;
    self->sessions[i].task = NULL;
    self->sessions[i].stack = NULL;
    self->sessions[i].client_fd = -1;
    self->sessions[i].client_ip[0] = '\0';
    self->sessions[i].last_error = 0;
    self->sessions[i].server = self;
  }

  self->response_sem = xSemaphoreCreateBinary();
  self->vfs_mutex = xSemaphoreCreateMutex();
  if (self->response_sem == NULL || self->vfs_mutex == NULL) {
    mp_raise_msg(&mp_type_MemoryError,
                MP_ERROR_TEXT("failed to allocate sftpd semaphores"));
  }

  return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t sftpd_server_start(mp_obj_t self_in) {
  sftpd_server_obj_t *self = MP_OBJ_TO_PTR(self_in);

  if (self->task != NULL) {
    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("already started"));
  }

  // Reclaim the previous run's PSRAM stack, if any -- self->task == NULL
  // (just confirmed above) means that task already reached its own
  // "self->task = NULL" line and won't touch this memory again, same
  // reasoning as this file's own session-slot reclaim above.
  if (self->stack != NULL) {
    heap_caps_free(self->stack);
    self->stack = NULL;
  }

  self->stop_request = false;
  self->last_error = 0;
  self->listen_fd = -1;
  self->state = SFTPD_STATE_IDLE;

  // PSRAM-backed stack for the listener task itself -- the per-session
  // tasks already get this (see sftpd_task()'s accept loop); the
  // listener never did.
  StackType_t *stack = heap_caps_malloc(SFTPD_TASK_STACK_WORDS * sizeof(StackType_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (stack == NULL) {
    stack = heap_caps_malloc(SFTPD_TASK_STACK_WORDS * sizeof(StackType_t),
                             MALLOC_CAP_8BIT);
  }
  if (stack == NULL) {
    mp_raise_msg(&mp_type_RuntimeError,
                MP_ERROR_TEXT("failed to allocate sftpd task stack"));
  }
  self->stack = stack;

  TaskHandle_t task = xTaskCreateStaticPinnedToCore(
      sftpd_task, "sftpd", SFTPD_TASK_STACK_WORDS, self, SFTPD_TASK_PRIORITY,
      stack, &self->task_buf,
      1 /* APP CPU -- leave PRO CPU/core 0 for MicroPython */);

  if (task == NULL) {
    heap_caps_free(stack);
    self->stack = NULL;
    mp_raise_msg(&mp_type_RuntimeError,
                MP_ERROR_TEXT("failed to start sftpd task"));
  }
  self->task = task;

  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(sftpd_server_start_obj, sftpd_server_start);

static mp_obj_t sftpd_server_stop(mp_obj_t self_in) {
  sftpd_server_obj_t *self = MP_OBJ_TO_PTR(self_in);
  self->stop_request = true;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(sftpd_server_stop_obj, sftpd_server_stop);

static mp_obj_t sftpd_server_status(mp_obj_t self_in) {
  sftpd_server_obj_t *self = MP_OBJ_TO_PTR(self_in);
  return mp_obj_new_int(self->state);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sftpd_server_status_obj, sftpd_server_status);

// Listener-level errors only (socket/bind/listen failures before any
// session existed). Use session_error_code(slot) for a specific
// connection's error.
static mp_obj_t sftpd_server_error_code(mp_obj_t self_in) {
  sftpd_server_obj_t *self = MP_OBJ_TO_PTR(self_in);
  return mp_obj_new_int(self->last_error);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sftpd_server_error_code_obj, sftpd_server_error_code);

static int sftpd_check_slot(mp_obj_t slot_in) {
  mp_int_t slot = mp_obj_get_int(slot_in);
  if (slot < 0 || slot >= SFTPD_MAX_SESSIONS) {
    mp_raise_ValueError(MP_ERROR_TEXT("bad session slot"));
  }
  return (int)slot;
}

// notify_cb's encoded slot argument is what sftpd.py passes in here to
// look up which IP connected/disconnected -- called on mp_task, so
// allocating the returned str is safe.
static mp_obj_t sftpd_server_session_ip(mp_obj_t self_in, mp_obj_t slot_in) {
  sftpd_server_obj_t *self = MP_OBJ_TO_PTR(self_in);
  int slot = sftpd_check_slot(slot_in);
  return mp_obj_new_str(self->sessions[slot].client_ip,
                        strlen(self->sessions[slot].client_ip));
}
static MP_DEFINE_CONST_FUN_OBJ_2(sftpd_server_session_ip_obj, sftpd_server_session_ip);

static mp_obj_t sftpd_server_session_error_code(mp_obj_t self_in, mp_obj_t slot_in) {
  sftpd_server_obj_t *self = MP_OBJ_TO_PTR(self_in);
  int slot = sftpd_check_slot(slot_in);
  return mp_obj_new_int(self->sessions[slot].last_error);
}
static MP_DEFINE_CONST_FUN_OBJ_2(sftpd_server_session_error_code_obj, sftpd_server_session_error_code);

static mp_obj_t sftpd_server_active_sessions(mp_obj_t self_in) {
  sftpd_server_obj_t *self = MP_OBJ_TO_PTR(self_in);
  int n = 0;
  for (int i = 0; i < SFTPD_MAX_SESSIONS; i++) {
    if (self->sessions[i].state == SFTPD_SLOT_ACTIVE) {
      n++;
    }
  }
  return mp_obj_new_int(n);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sftpd_server_active_sessions_obj, sftpd_server_active_sessions);

// Same mp_sched_schedule()-based design as modsshd.c's set_notify().
static mp_obj_t sftpd_server_set_notify(mp_obj_t self_in, mp_obj_t cb) {
  sftpd_server_obj_t *self = MP_OBJ_TO_PTR(self_in);
  self->notify_cb = cb;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sftpd_server_set_notify_obj, sftpd_server_set_notify);

// Registers the VFS servicer -- invoked as callback(self) on mp_task
// whenever a myFilesystem.h hook needs a real Python VFS operation done.
static mp_obj_t sftpd_server_set_vfs_service(mp_obj_t self_in, mp_obj_t cb) {
  sftpd_server_obj_t *self = MP_OBJ_TO_PTR(self_in);
  self->vfs_service_cb = cb;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sftpd_server_set_vfs_service_obj, sftpd_server_set_vfs_service);

// Returns (op, path_a, path_b, flags, handle, offset, data) describing
// the currently pending VFS op -- only meaningful from within a
// set_vfs_service() callback (mp_obj_new_str/_bytes allocate, so this
// must never be called from the background task).
static mp_obj_t sftpd_server_request(mp_obj_t self_in) {
  sftpd_server_obj_t *self = MP_OBJ_TO_PTR(self_in);
  mp_obj_t items[7] = {
      mp_obj_new_int(self->op),
      mp_obj_new_str(self->path_a, strlen(self->path_a)),
      mp_obj_new_str(self->path_b, strlen(self->path_b)),
      mp_obj_new_int(self->flags),
      mp_obj_new_int(self->handle),
      mp_obj_new_int_from_uint(self->ofst),
      mp_obj_new_bytes(self->scratch, self->scratch_sz),
  };
  return mp_obj_new_tuple(7, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sftpd_server_request_obj, sftpd_server_request);

// respond(ret, handle, data, mode, size, mtime, name, eof) -- reports
// the result of the pending VFS op back to the background task and
// wakes it. Field meaning depends on which op is pending (see sftpd.py's
// servicer for the mapping).
static mp_obj_t sftpd_server_respond(size_t n_args, const mp_obj_t *args) {
  sftpd_server_obj_t *self = MP_OBJ_TO_PTR(args[0]);

  self->op_ret = mp_obj_get_int(args[1]);
  self->handle = mp_obj_get_int(args[2]);

  mp_buffer_info_t data_buf;
  mp_get_buffer_raise(args[3], &data_buf, MP_BUFFER_READ);
  size_t n = data_buf.len < sizeof(self->scratch) ? data_buf.len : sizeof(self->scratch);
  memcpy(self->scratch, data_buf.buf, n);
  self->scratch_sz = (word32)n;

  self->resp_mode = (word32)mp_obj_get_int(args[4]);
  self->resp_size = (word32)mp_obj_get_int(args[5]); // also: WRITE's byte count
  self->resp_mtime = (word32)mp_obj_get_int(args[6]);

  const char *name = mp_obj_str_get_str(args[7]);
  strncpy(self->name, name, sizeof(self->name) - 1);
  self->name[sizeof(self->name) - 1] = '\0';

  self->eof = mp_obj_is_true(args[8]);

  xSemaphoreGive(self->response_sem);
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sftpd_server_respond_obj, 9, 9, sftpd_server_respond);

static const mp_rom_map_elem_t sftpd_server_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&sftpd_server_start_obj)},
    {MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&sftpd_server_stop_obj)},
    {MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&sftpd_server_status_obj)},
    {MP_ROM_QSTR(MP_QSTR_error_code), MP_ROM_PTR(&sftpd_server_error_code_obj)},
    {MP_ROM_QSTR(MP_QSTR_session_ip), MP_ROM_PTR(&sftpd_server_session_ip_obj)},
    {MP_ROM_QSTR(MP_QSTR_session_error_code), MP_ROM_PTR(&sftpd_server_session_error_code_obj)},
    {MP_ROM_QSTR(MP_QSTR_active_sessions), MP_ROM_PTR(&sftpd_server_active_sessions_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_notify), MP_ROM_PTR(&sftpd_server_set_notify_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_vfs_service), MP_ROM_PTR(&sftpd_server_set_vfs_service_obj)},
    {MP_ROM_QSTR(MP_QSTR_request), MP_ROM_PTR(&sftpd_server_request_obj)},
    {MP_ROM_QSTR(MP_QSTR_respond), MP_ROM_PTR(&sftpd_server_respond_obj)},
};
static MP_DEFINE_CONST_DICT(sftpd_server_locals_dict, sftpd_server_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(sftpd_server_type, MP_QSTR_Server, MP_TYPE_FLAG_NONE,
                         make_new, sftpd_server_make_new, locals_dict,
                         &sftpd_server_locals_dict);

static const mp_rom_map_elem_t modsftpd_module_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_modsftpd)},
    {MP_ROM_QSTR(MP_QSTR_Server), MP_ROM_PTR(&sftpd_server_type)},
    {MP_ROM_QSTR(MP_QSTR_IDLE), MP_ROM_INT(SFTPD_STATE_IDLE)},
    {MP_ROM_QSTR(MP_QSTR_LISTENING), MP_ROM_INT(SFTPD_STATE_LISTENING)},
    {MP_ROM_QSTR(MP_QSTR_STOPPED), MP_ROM_INT(SFTPD_STATE_STOPPED)},
    {MP_ROM_QSTR(MP_QSTR_OP_STAT), MP_ROM_INT(SFTPD_OP_STAT)},
    {MP_ROM_QSTR(MP_QSTR_OP_LSTAT), MP_ROM_INT(SFTPD_OP_LSTAT)},
    {MP_ROM_QSTR(MP_QSTR_OP_OPEN), MP_ROM_INT(SFTPD_OP_OPEN)},
    {MP_ROM_QSTR(MP_QSTR_OP_READ), MP_ROM_INT(SFTPD_OP_READ)},
    {MP_ROM_QSTR(MP_QSTR_OP_WRITE), MP_ROM_INT(SFTPD_OP_WRITE)},
    {MP_ROM_QSTR(MP_QSTR_OP_CLOSE), MP_ROM_INT(SFTPD_OP_CLOSE)},
    {MP_ROM_QSTR(MP_QSTR_OP_OPENDIR), MP_ROM_INT(SFTPD_OP_OPENDIR)},
    {MP_ROM_QSTR(MP_QSTR_OP_READDIR), MP_ROM_INT(SFTPD_OP_READDIR)},
    {MP_ROM_QSTR(MP_QSTR_OP_CLOSEDIR), MP_ROM_INT(SFTPD_OP_CLOSEDIR)},
    {MP_ROM_QSTR(MP_QSTR_OP_MKDIR), MP_ROM_INT(SFTPD_OP_MKDIR)},
    {MP_ROM_QSTR(MP_QSTR_OP_RMDIR), MP_ROM_INT(SFTPD_OP_RMDIR)},
    {MP_ROM_QSTR(MP_QSTR_OP_REMOVE), MP_ROM_INT(SFTPD_OP_REMOVE)},
    {MP_ROM_QSTR(MP_QSTR_OP_RENAME), MP_ROM_INT(SFTPD_OP_RENAME)},
    {MP_ROM_QSTR(MP_QSTR_OP_GETCWD), MP_ROM_INT(SFTPD_OP_GETCWD)},
    {MP_ROM_QSTR(MP_QSTR_FXF_READ), MP_ROM_INT(WOLFSSH_O_RDONLY)},
    {MP_ROM_QSTR(MP_QSTR_FXF_WRITE), MP_ROM_INT(WOLFSSH_O_WRONLY)},
    {MP_ROM_QSTR(MP_QSTR_FXF_RDWR), MP_ROM_INT(WOLFSSH_O_RDWR)},
    {MP_ROM_QSTR(MP_QSTR_FXF_APPEND), MP_ROM_INT(WOLFSSH_O_APPEND)},
    {MP_ROM_QSTR(MP_QSTR_FXF_CREAT), MP_ROM_INT(WOLFSSH_O_CREAT)},
    {MP_ROM_QSTR(MP_QSTR_FXF_TRUNC), MP_ROM_INT(WOLFSSH_O_TRUNC)},
    {MP_ROM_QSTR(MP_QSTR_FXF_EXCL), MP_ROM_INT(WOLFSSH_O_EXCL)},
    // sftpd.py's READ servicer caps its own read() at this so the two
    // sides can never silently disagree about the shared scratch
    // buffer's size.
    {MP_ROM_QSTR(MP_QSTR_SCRATCH_SZ), MP_ROM_INT(SFTPD_SCRATCH_SZ)},
};
static MP_DEFINE_CONST_DICT(modsftpd_module_globals, modsftpd_module_globals_table);

const mp_obj_module_t modsftpd_module = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&modsftpd_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_modsftpd, modsftpd_module);
