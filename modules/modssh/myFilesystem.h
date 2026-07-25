/*
 * myFilesystem.h -- wolfSSH's WOLFSSH_USER_FILESYSTEM hook surface,
 * implemented by modsftpd.c (see that file's header comment for the
 * full architecture). Included from wolfssh/port.h, very early in the
 * wolfssh/wolfsftp.h include chain -- before WS_SFTP_FILEATRB's own
 * typedef later in that same file, hence the forward-declared
 * `struct WS_SFTP_FILEATRB` below rather than including wolfsftp.h back
 * (which would be circular anyway). A pointer to an incomplete struct
 * type is legal in a prototype; modsftpd.c sees the real, complete type
 * by the time it actually defines these functions.
 *
 * Verified against src/wolfsftp.c directly (grep every Recv* handler's
 * actual macro usage) -- caught a real gap the first time round: the
 * WFILE/WFOPEN stdio family genuinely is Get()/Put()-only (client-side
 * convenience wrappers neither modsftp.c nor modsftpd.c ever calls) and
 * stays stubbed to dead values below, but wolfSSH_SFTP_RecvRealPath()
 * (a real server-side handler, reached whenever a client resolves "."
 * or its initial directory) also needs WGETCWD -- missed on the first
 * pass because that function is `static`, which an earlier grep
 * pattern didn't account for. WGETCWD is wired through properly (an
 * RPC to mp_task for the live os.getcwd()) rather than stubbed, since
 * it directly determines what "rooted the same way the physical shell
 * sees it" (the confirmed design decision) actually resolves to.
 * SFTP_GetAttributes()/_Handle() are real C functions (not macros) --
 * wolfsftp.c's own forward declaration for them is `#ifndef
 * WOLFSSH_USER_FILESYSTEM`, i.e. skipped for us, meaning it expects an
 * externally-linked definition instead of its own per-RTOS-branch
 * static one. SFTP_SetFileAttributes/SFTP_SetMode (chmod/utime) are
 * NOT gated the same way -- they get wolfSSH's own real generic-POSIX
 * bodies regardless, which just call WCHMOD/WSETTIME below (safe
 * no-ops here, matching wolfSSH's own existing WSETTIME-is-a-no-op
 * precedent even on real builds -- chmod/utime aren't meaningful for
 * most MicroPython VFS backends anyway).
 */
#ifndef MODSFTPD_MY_FILESYSTEM_H
#define MODSFTPD_MY_FILESYSTEM_H

#include <dirent.h>

typedef int WFD;
typedef int WDIR;
typedef int WFILE; /* dummy -- see header comment, WFOPEN family is dead code */

/* Values are ours to choose -- nothing outside modsftpd.c's own
 * modsftpd_wopen() ever inspects them, so they don't need to match
 * real POSIX O_* bit values, just be distinct. */
#define WOLFSSH_O_RDONLY 0x0001
#define WOLFSSH_O_WRONLY 0x0002
#define WOLFSSH_O_RDWR   0x0004
#define WOLFSSH_O_APPEND 0x0008
#define WOLFSSH_O_CREAT  0x0010
#define WOLFSSH_O_TRUNC  0x0020
#define WOLFSSH_O_EXCL   0x0040

#define WS_DELIM '/'

struct WS_SFTP_FILEATRB;

#ifdef __cplusplus
extern "C" {
#endif

int SFTP_GetAttributes(void *fs, const char *fileName,
                       struct WS_SFTP_FILEATRB *atr, unsigned char noFollow,
                       void *heap);
int SFTP_GetAttributes_Handle(void *ssh, unsigned char *handle, int handleSz,
                              struct WS_SFTP_FILEATRB *atr);

int modsftpd_wopen(void *fs, const char *path, int flags, unsigned int mode);
int modsftpd_wpread(void *fs, WFD fd, unsigned char *buf, unsigned int sz,
                    const unsigned int *ofst);
int modsftpd_wpwrite(void *fs, WFD fd, unsigned char *buf, unsigned int sz,
                     const unsigned int *ofst);
int modsftpd_wclose(void *fs, WFD fd);
int modsftpd_wopendir(void *fs, void *heap, WDIR *ctx, const char *path);
struct dirent *modsftpd_wreaddir(void *fs, WDIR *ctx);
int modsftpd_wclosedir(void *fs, WDIR *ctx);
int modsftpd_wmkdir(void *fs, const char *path, unsigned int mode);
int modsftpd_wrmdir(void *fs, const char *path);
int modsftpd_wremove(void *fs, const char *path);
int modsftpd_wrename(void *fs, const char *oldPath, const char *newPath);
char *modsftpd_wgetcwd(void *fs, char *buf, unsigned int sz);

#ifdef __cplusplus
}
#endif

#define WOPEN(fs, f, m, p) modsftpd_wopen((fs), (f), (m), (p))
#define WPREAD(fs, fd, b, s, o) modsftpd_wpread((fs), (fd), (b), (s), (o))
#define WPWRITE(fs, fd, b, s, o) modsftpd_wpwrite((fs), (fd), (b), (s), (o))
#define WCLOSE(fs, fd) modsftpd_wclose((fs), (fd))
#define WOPENDIR(fs, h, c, d) modsftpd_wopendir((fs), (h), (c), (d))
#define WREADDIR(fs, d) modsftpd_wreaddir((fs), (d))
#define WCLOSEDIR(fs, d) modsftpd_wclosedir((fs), (d))
#define WMKDIR(fs, p, m) modsftpd_wmkdir((fs), (p), (m))
#define WRMDIR(fs, d) modsftpd_wrmdir((fs), (d))
#define WREMOVE(fs, d) modsftpd_wremove((fs), (d))
#define WRENAME(fs, o, n) modsftpd_wrename((fs), (o), (n))
#define WGETCWD(fs, r, rSz) modsftpd_wgetcwd((fs), (r), (rSz))

/* Safe no-ops -- see this file's header comment. */
#define WCHMOD(fs, f, m) (0)
#define WFCHMOD(fs, fd, m) (0)
#define WSETTIME(fs, f, a, m) (0)
#define WFSETTIME(fs, fd, a, m) (0)

/* Dead code -- wolfSSH_SFTP_Get()/_Put() (the only callers) are never
 * invoked by either modsftp.c or modsftpd.c. Values are irrelevant;
 * these just need to exist, with the right arity, so wolfsftp.c
 * compiles (it references them unconditionally in its own source, not
 * gated behind whether an app ever calls Get/Put). Matches the exact
 * approach originally used before WOLFSSH_USER_FILESYSTEM was found to
 * be necessary -- see micropython.cmake's comment history.
 *
 * WFOPEN/WFREAD/WFWRITE are always used in an expression context at
 * their call sites (assigned or compared), so a plain int value is
 * fine. WFCLOSE/WFSEEK are called as bare statements (no assignment) --
 * confirmed via the actual call sites -- so they need ((void)0), not a
 * discarded int, or -Werror=unused-value trips on the "statement with
 * no effect". */
#define WFOPEN(fs, f, fn, m) (-1)
#define WFCLOSE(fs, f) ((void)0)
#define WFREAD(fs, b, s, a, f) (0)
#define WFWRITE(fs, b, s, a, f) (0)
#define WFSEEK(fs, s, o, w) ((void)0)

#endif /* MODSFTPD_MY_FILESYSTEM_H */
