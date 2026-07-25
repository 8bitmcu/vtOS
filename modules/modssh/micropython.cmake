# Create an INTERFACE library for our C module.
add_library(usermod_modssh INTERFACE)

# Add our source files to the lib. modsshd.c (the SSH server),
# modsftp.c (the SFTP client), and modsftpd.c (the SFTP server), each
# registered as their own module -- see their header comments -- live
# here too rather than in separate modules so they pick up all the
# wolfSSH vendor patches below automatically.
target_sources(usermod_modssh INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/modssh.c
    ${CMAKE_CURRENT_LIST_DIR}/modsshd.c
    ${CMAKE_CURRENT_LIST_DIR}/modsftp.c
    ${CMAKE_CURRENT_LIST_DIR}/modsftpd.c)

# Add the current directory as an include directory, plus tdeck_i2s for
# the shared ring_buf.h (same cross-task producer/consumer buffer already
# used by audioplayer.c/audiorecorder.c), plus our own wolfssl_shim (see
# below).
target_include_directories(usermod_modssh INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../tdeck_i2s
    ${CMAKE_CURRENT_LIST_DIR}/wolfssl_shim
)

# Link against the wolfSSL/wolfSSH ESP-IDF components (see
# modules/idf_component.yml and boards/LILYGO_T_DECK/sdkconfig.board's
# CONFIG_ESP_ENABLE_WOLFSSH).
#
# Actual on-disk layout of wolfssl__wolfssl (verified directly, not
# assumed): COMPONENT_DIR/wolfssl/{wolfcrypt,openssl} for <wolfssl/...>
# includes, COMPONENT_DIR/include for user_settings.h. There is no
# port/ directory and no generated options.h in this packaging -- see
# wolfssl_shim/wolfssl/options.h, which redirects wolfssh's unconditional
# `#include <wolfssl/options.h>` to the real user_settings.h instead.
idf_component_get_property(wolfssl_dir wolfssl__wolfssl COMPONENT_DIR)
idf_component_get_property(wolfssl_lib wolfssl__wolfssl COMPONENT_LIB)
target_include_directories(usermod_modssh INTERFACE
    ${wolfssl_dir}
    ${wolfssl_dir}/include
)
target_link_libraries(usermod_modssh INTERFACE ${wolfssl_lib})

idf_component_get_property(wolfssh_dir wolfssl__wolfssh COMPONENT_DIR)
idf_component_get_property(wolfssh_lib wolfssl__wolfssh COMPONENT_LIB)
target_include_directories(usermod_modssh INTERFACE
    ${wolfssh_dir}
)
target_link_libraries(usermod_modssh INTERFACE ${wolfssh_lib})

# wolfSSH's client connect state machine (src/ssh.c,
# CONNECT_CLIENT_CHANNEL_AGENT_REQUEST_SENT case) only sends a pty-req
# (SendChannelTerminalRequest(), needed for our interactive shell use --
# see modssh.c's wolfSSH_SetChannelType(..., WOLFSSH_SESSION_TERMINAL,
# ...)) when built with `!defined(NO_FILESYSTEM)` -- and
# SendChannelTerminalRequest's own *definition*, plus its CreateMode()
# helper, are wrapped in the exact same check in src/internal.c, so
# patching just the ssh.c call site alone compiles but leaves the
# function itself out of the binary (undefined reference at link time).
# This project requires NO_FILESYSTEM globally
# (see wolfssl__wolfssl's user_settings.h) for many other, genuinely
# filesystem-dependent wolfSSL/wolfCrypt paths, so undefining it
# project-wide isn't viable -- but this specific code path has no hard
# filesystem dependency: GetTerminalInfo() already falls back to sane
# 80x24 defaults without HAVE_SYS_IOCTL_H, and getenv("TERM") works fine
# on this target's newlib. The one genuinely POSIX-specific bit --
# CreateMode()'s tcgetattr(STDIN_FILENO, ...) call, which wouldn't make
# sense here anyway (no real controlling tty on this target, and it'd be
# querying this device's own local console settings rather than
# anything about the remote session) -- is already separately guarded by
# `!defined(NO_TERMIOS)` with a safe dummy-38400-baud fallback, so
# defining NO_TERMIOS below sidesteps it cleanly rather than needing yet
# another patch. Patch both call sites (ssh.c and internal.c) rather
# than the function/global macro. Idempotent (sed's search text no
# longer matches once patched, so a second run is a silent no-op) so
# this reapplies safely on every fresh `make init` re-fetch of the
# component.
execute_process(
    COMMAND sed -i
        "s/#if defined(WOLFSSH_TERM) && !defined(NO_FILESYSTEM)/#if defined(WOLFSSH_TERM)/"
        ${wolfssh_dir}/src/ssh.c
)
execute_process(
    COMMAND sed -i
        "s/#if defined(WOLFSSH_TERM) && !defined(NO_FILESYSTEM)/#if defined(WOLFSSH_TERM)/"
        ${wolfssh_dir}/src/internal.c
)
target_compile_definitions(${wolfssh_lib} PRIVATE NO_TERMIOS)

# wolfSSH's client always prefers keyboard-interactive over password
# whenever a server offers both, with no fallback -- see
# patch_wolfssh_auth_priority.py for the full rationale (many real
# servers' keyboard-interactive backends reject wolfSSH's bare
# negotiation outright, breaking auth even though plain password would
# have worked). Patches src/internal.c to skip keyboard-interactive
# whenever password is also on offer.
execute_process(
    COMMAND python3
        ${CMAKE_CURRENT_LIST_DIR}/patch_wolfssh_auth_priority.py
        ${wolfssh_dir}
)

# modsshd.c (the SSH server) calls wolfSSH_MakeEcdsaKey() to generate its
# host key -- verified directly against src/keygen.c: both
# wolfSSH_MakeRsaKey() and wolfSSH_MakeEcdsaKey() live inside one shared
# `#ifdef WOLFSSH_KEYGEN` / `#ifdef WOLFSSL_KEY_GEN` block (lines 52-194),
# so wolfSSH_MakeEcdsaKey's body doesn't compile at all -- not even a
# stub -- unless both macros are defined, even though its own code path
# only touches ECC. Neither is defined by this project's
# user_settings.h (the one `#define WOLFSSL_KEY_GEN` in there is inside
# `#if 0`, dead text). Both are needed on wolfssh_lib only -- keygen.c is
# part of that component, not wolfssl_lib's. (wc_EccKeyToDer(), the
# DER-export call wolfSSH_MakeEcdsaKey uses to serialize the generated
# key, is separately gated by HAVE_ECC_KEY_EXPORT in wolfssl's own
# asn.c -- confirmed already enabled by default whenever HAVE_ECC is set,
# see settings.h -- so wolfssl_lib needs no define here.)
target_compile_definitions(${wolfssh_lib} PRIVATE WOLFSSH_KEYGEN)
target_compile_definitions(${wolfssh_lib} PRIVATE WOLFSSL_KEY_GEN)

# modsftp.c (the SFTP client) needs WOLFSSH_SFTP for src/wolfsftp.c to
# compile at all -- and that file is much more than the high-level
# wolfSSH_SFTP_Get()/_Put() modsftp.c deliberately never calls (see its
# header comment): it also contains wolfSSH's SERVER-side SFTP receive
# handlers (RecvOpen/RecvMKDIR/RecvRead/SFTP_GetAttributes/etc, live in
# this build since NO_WOLFSSH_SERVER isn't defined -- modsshd.c needs
# server support), which use a whole family of local-filesystem macros
# (WFD/WDIR/WSTAT_T/WOPEN/WSTAT/WCHMOD/WRENAME/...). Confirmed directly
# against wolfssh/port.h: these all live behind
# `#if (WOLFSSH_SFTP || WOLFSSH_SCP || WOLFSSH_SSHD) &&
#      !NO_WOLFSSH_SERVER && !NO_FILESYSTEM` (~line 585) and an outer
# `#ifdef NO_FILESYSTEM` wrapping the separate WFOPEN/WFCLOSE/WFREAD/
# WFWRITE/WFSTAT/WCHMOD/WMKDIR block (~line 104) -- both real, working
# POSIX wrappers (fopen/open/stat/opendir/...) in the generic non-RTOS
# branch, entirely unavailable to wolfsftp.c while NO_FILESYSTEM is
# defined (which this project's user_settings.h does globally, for
# wolfCrypt's own unrelated reasons). Hand-stubbing ~20 macros (some
# needing real POSIX-compatible struct layouts, e.g. WSTAT_T) to dead-end
# values would be far more fragile than just letting wolfSSH's own,
# already-correct generic-POSIX definitions apply -- ESP-IDF's newlib
# genuinely provides fopen/open/stat/opendir etc, so this compiles to
# real (if never-actually-called-by-us) libc functions, all harmless
# dead code from modsftp.c's perspective since it only ever drives the
# low-level Open/SendReadPacket/SendWritePacket/Close/LS/etc primitives.
#
# Patches wolfssh/port.h AND src/port.c (declarations and definitions,
# respectively -- see patch_wolfssh_sftp_filesystem.py for the full
# rationale and exact guards involved) rather than stubbing at the
# compiler-definition level: all three guards are scoped entirely to
# wolfSSH's own SFTP/SCP/SSHD file-I/O macros, not wolfCrypt's separate,
# broader NO_FILESYSTEM concerns elsewhere in wolfssl_lib, so this can't
# affect anything modssh.c/modsshd.c already depend on. Plain text
# substitution rather than sed -- see that script's header comment for
# why. Idempotent same as the other patch scripts in this file.
target_compile_definitions(${wolfssh_lib} PRIVATE WOLFSSH_SFTP)
execute_process(
    COMMAND python3
        ${CMAKE_CURRENT_LIST_DIR}/patch_wolfssh_sftp_filesystem.py
        ${wolfssh_dir}
)

# modsftpd.c (the SFTP server) needs wolfSSH's own server-side SFTP
# Recv* handlers to call into MicroPython's real VFS, not a real
# on-target POSIX filesystem (there isn't one -- see modsftpd.c's
# header comment). wolfssh/port.h supports exactly this via
# WOLFSSH_USER_FILESYSTEM, which -- confirmed directly -- has its own
# dedicated `#elif defined(WOLFSSH_USER_FILESYSTEM)` branch in BOTH of
# the file-I/O macro chains the patches above target (the WFOPEN-family
# one and the WFD/WDIR-family one), each just `#include "myFilesystem.h"`.
# Since an #elif chain only ever selects its first matching branch, this
# preempts the generic-POSIX branch the patches above unlock -- meaning
# those patches become dormant/unused now (harmless: the client's own
# code never touches these macros either way, confirmed when they were
# first added, so nothing regresses either directly using or continuing
# past them).
#
# myFilesystem.h (modules/modssh/myFilesystem.h) is found via a plain
# "" include -- needs modules/modssh/ on wolfssh_lib's OWN include
# path, not just usermod_modssh's (this file's existing
# target_include_directories(usermod_modssh ...) calls only cover our
# own compilation units, not wolfssh_lib's, since port.h is compiled as
# part of that separate component library).
target_compile_definitions(${wolfssh_lib} PRIVATE WOLFSSH_USER_FILESYSTEM)
target_include_directories(${wolfssh_lib} PRIVATE ${CMAKE_CURRENT_LIST_DIR})

# modsftpd.c itself also needs WOLFSSH_USER_FILESYSTEM -- confirmed via
# an actual build that skipping this was a real bug, not just belt and
# suspenders: modsftpd.c is compiled as part of usermod_modssh/usermod,
# a *different* CMake target from wolfssh_lib, so it doesn't inherit
# wolfssh_lib's PRIVATE define above. Without it, modsftpd.c's own
# `#include <wolfssh/ssh.h>` pulls in port.h for the first time (within
# that translation unit) with WOLFSSH_USER_FILESYSTEM invisible, so
# port.h takes the generic real-POSIX branch instead (defining real
# fopen/chmod/WFILE=FILE/etc) -- and modsftpd.c's own later
# `#include "myFilesystem.h"` then redefines everything a second time,
# which is how `typedef int WFILE;` ended up colliding with the real
# FILE typedef. usermod_modssh is INTERFACE-only, so this INTERFACE
# define propagates to its sources (this one and its siblings, though
# only modsftpd.c cares) via the target_link_libraries(usermod
# INTERFACE usermod_modssh) call at the bottom of this file.
target_compile_definitions(usermod_modssh INTERFACE WOLFSSH_USER_FILESYSTEM)

# wolfSSH_SFTP_accept()'s SFTP_BEGIN/SFTP_EXT case wipes all SFTP receive
# state (wolfSSH_SFTP_ClearState(ssh, STATE_ID_ALL)) on ANY non-success
# return from SFTP_ServerRecvInit() except WS_WANT_READ/WS_WANT_WRITE --
# WS_CHAN_RXD ("channel data received", a normal transient status, not a
# fault -- see wolfssh/error.h) isn't exempted. Confirmed the hard way:
# a real SFTP client's INIT packet reliably triggers WS_CHAN_RXD at least
# once, and every time it does, this clears state and restarts
# SFTP_ServerRecvInit() from scratch -- forever, since the restart hits
# the exact same WS_CHAN_RXD again before ever accumulating a complete
# packet. This is the server-side counterpart of a bug wolfSSH's own
# v1.4.21 fixed on the *client* side (SFTP_ClientRecvInit(), PR 846,
# "SFTP fix for init to handle channel data") -- our vendored 1.4.20
# predates that fix, and it was never applied to this server-side
# function (a much less exercised code path). Python string replace, not
# sed -- see patch_wolfssh_sftp_filesystem.py's header comment for why
# (the replacement text itself contains && here, which sed's s///
# replacement syntax would misinterpret as "insert matched text").
execute_process(
    COMMAND python3
        ${CMAKE_CURRENT_LIST_DIR}/patch_wolfssh_sftp_accept_chan_rxd.py
        ${wolfssh_dir}
)

# wolfSSH_SFTP_read()'s STATE_RECV_DO case (the per-message read loop, not
# the one-time init handled above) has the exact same "doesn't exempt
# WS_CHAN_RXD" bug -- but here it desyncs the channel's read cursor from
# wolfSSH's own SFTP parsing state, since the header has already been
# consumed by the time the payload read fails: a retry restarts from
# STATE_RECV_READ and misparses payload bytes as a new header, producing a
# bogus declared length that gets passed straight to WMALLOC(), which fails
# with WS_MEMORY_E. Confirmed against a real device: reproducible on any
# upload whose WRITE payload needs more than one channel-data delivery to
# fully arrive (small files that land in a single delivery never hit this).
# See patch_wolfssh_sftp_read_chan_rxd.py for the full trace.
execute_process(
    COMMAND python3
        ${CMAKE_CURRENT_LIST_DIR}/patch_wolfssh_sftp_read_chan_rxd.py
        ${wolfssh_dir}
)

# src/wolfsftp.c's SFTP_CreateLongName() tests file type via raw bit
# overlap (`tmp & FILEATRB_PER_LINK`) instead of masking to the type
# field and comparing exactly -- FILEATRB_PER_LINK (0120000) shares a
# bit with FILEATRB_PER_FILE (0100000), so every plain regular file
# (mode 0100000, exactly what MicroPython's os.stat() returns for a
# file with no extra permission bits) spuriously matches the symlink
# check and gets shown as 'l-------' in the longname string SFTP-v3
# clients like FileZilla display for their permission column. Confirmed
# against a real device. See patch_wolfssh_longname_type_bits.py.
execute_process(
    COMMAND python3
        ${CMAKE_CURRENT_LIST_DIR}/patch_wolfssh_longname_type_bits.py
        ${wolfssh_dir}
)

# wolfssl/wolfcrypt/settings.h (via port/Espressif/esp-sdk-lib.h) hard
# #errors unless WOLFSSL_USER_SETTINGS is defined -- neither component's
# own build defines this for itself, only (maybe) for dependents, so
# wolfSSL's and wolfSSH's own source files fail to compile without it.
# Applied directly to both COMPONENT_LIB targets (not just our own
# usermod_modssh INTERFACE) so it reaches their own compilation units, not
# just ours.
target_compile_definitions(${wolfssl_lib} PUBLIC WOLFSSL_USER_SETTINGS)
target_compile_definitions(${wolfssh_lib} PUBLIC WOLFSSL_USER_SETTINGS)

# wolfssl/wolfcrypt/settings.h's own FREERTOS/WOLFSSL_ESPIDF block
# (confirmed directly against the vendored source, ~line 1487) unconditionally
# defines XMALLOC/XFREE/XREALLOC as pvPortMalloc()/vPortFree()/realloc() --
# and on this board, that only ever draws from internal SRAM, which is tight
# enough under normal SFTP session load (confirmed on a real device: ~42KB
# free, ~22KB largest contiguous block, mid-session) that a single WMALLOC()
# for one large incoming SFTP WRITE message (~32KB, matching
# WOLFSSH_MAX_SFTP_RW) reliably fails with WS_MEMORY_E, even though 8MB of
# PSRAM sits idle right next to it. (Two independent large-buffer WMALLOC()
# sites were found failing this way: wolfsftp.c's own SFTP message buffer,
# and internal.c's GrowBuffer() for the raw SSH channel-data packet
# underneath it -- fixing the allocator itself, not each call site, is what
# covers both.)
#
# settings.h's block only steps aside for XMALLOC_USER (not the
# XMALLOC_OVERRIDE mechanism most other embedded ports use, which is checked
# later in wolfcrypt/types.h -- too late, settings.h's own definitions have
# already won by then). XMALLOC_USER requires real extern function
# definitions matching exact signatures, not macros -- see
# wolfssl_shim/psram_alloc.c, added to wolfssl_lib's own sources below so it
# links into the same component. Applied to both libs since wolfssh_lib's
# own source (wolfsftp.c, internal.c) references the same three symbols.
target_compile_definitions(${wolfssl_lib} PRIVATE XMALLOC_USER)
target_compile_definitions(${wolfssh_lib} PRIVATE XMALLOC_USER)
target_sources(${wolfssl_lib} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/wolfssl_shim/psram_alloc.c
)

# Cleanup for an earlier, broken attempt at the fix above (see
# patch_wolfssl_psram_alloc.py) that appended a now-removed block directly
# to user_settings.h -- harmless/no-op once a build has picked up the
# removal, kept only so a component cache that already has the old,
# broken append self-heals on the next build without needing a manual
# cache clear.
execute_process(
    COMMAND python3
        ${CMAKE_CURRENT_LIST_DIR}/patch_wolfssl_psram_alloc.py
        ${wolfssl_dir}
)

# wolfssl__wolfssl's vendored user_settings.h caps DEFAULT_WINDOW_SZ at
# 2000 bytes inside its ESP_ENABLE_WOLFSSH block -- an Espressif-example
# default ("The default SSH Windows size is massive for an embedded
# target. Limit it:"), not something chosen for this project's own SFTP
# use case. Confirmed as the root cause of multi-MB SFTP upload hangs on
# a real device: a temporary diagnostic accessor for the channel's
# receive window showed it cycling in the ~200-2000 byte range throughout
# a transfer. A single 32768-byte SFTP WRITE (WOLFSSH_MAX_SFTP_RW) needs
# roughly 16 WINDOW_ADJUST round-trips to get through at that size; a multi-MB
# upload needs thousands, and wolfSSH's own window-replenish trigger
# (_UpdateChannelWindow(), src/ssh.c) only fires once the window's
# already nearly/fully exhausted -- living permanently at that edge makes
# eventually missing one close to certain. Raises it back to wolfSSH's
# own upstream default (128KB) -- comfortably affordable now that WMALLOC
# routes through PSRAM (see psram_alloc.c above), which is what backs the
# channel inputBuffer this window size sizes. See
# patch_wolfssl_window_sz.py for the full trace.
execute_process(
    COMMAND python3
        ${CMAKE_CURRENT_LIST_DIR}/patch_wolfssl_window_sz.py
        ${wolfssl_dir}
)

# wolfSSL's fast-math bignum backend (wolfcrypt/src/tfm.c) exports a
# global `mp_init(mp_int *)` -- pure coincidence of naming convention
# ("mp" = multi-precision there, vs "MicroPython" in py/runtime.c, which
# also exports a global `mp_init(void)` for VM startup). Both are real,
# needed, unrelated symbols, so this is a link-time collision, not a bug
# in either project. Can't touch either vendor's source, so rename
# wolfSSL's via a textual macro at the compiler level -- applied PRIVATE
# to just these two component targets (every wolfSSL/wolfSSH source file
# that declares or calls mp_init picks up the same rename, so they stay
# internally consistent) and never to usermod_modssh/modssh.c or MicroPython
# itself, which must keep seeing the real mp_init.
target_compile_definitions(${wolfssl_lib} PRIVATE mp_init=wc_fastmath_mp_init)
target_compile_definitions(${wolfssh_lib} PRIVATE mp_init=wc_fastmath_mp_init)

# Link our INTERFACE library to the usermod target.
target_link_libraries(usermod INTERFACE usermod_modssh)
