#!/usr/bin/env python3
# Patches src/wolfsftp.c's wolfSSH_SFTP_read(): its STATE_RECV_DO case wipes
# all SFTP receive state (wolfSSH_SFTP_ClearState(ssh, STATE_ID_RECV)) on any
# non-success return from reading a packet's body except WS_WANT_READ/
# WS_WANT_WRITE -- WS_CHAN_RXD isn't exempted here either, same bug class as
# patch_wolfssh_sftp_accept_chan_rxd.py, just one state machine over (the
# per-message read loop instead of the one-time SFTP init), and with a much
# nastier failure mode.
#
# Confirmed against a real device: uploading a file large enough that a
# single WRITE's payload needs more than one channel-data delivery to fully
# arrive reliably triggers WS_CHAN_RXD partway through STATE_RECV_DO's
# wolfSSH_SFTP_buffer_read() (small files whose WRITE payload always lands in
# one delivery never hit this). That wipes ssh->recvState and returns --  but
# per wolfSSH_stream_read()'s own internal handling (src/ssh.c), ssh->error
# stays WS_CHAN_RXD even after the transient condition resolves, so our own
# retry loop (correctly, by that reading) calls wolfSSH_SFTP_read() again.
# With recvState gone, that restarts from STATE_RECV_READ -- re-parsing a
# fresh 9-byte header -- but the wire's read cursor inside wolfSSH's own
# channel input buffer is already positioned mid-payload from the *previous*
# (discarded) attempt, since the header was already consumed before the
# payload read that failed. The "header" this retry parses is actually
# arbitrary file-content bytes, so the declared packet length is bogus --
# usually enormous -- and gets passed straight to wolfSSH_SFTP_buffer_create()
# -> WMALLOC(), which fails with WS_MEMORY_E. This matches, exactly, a
# reproducible WS_MEMORY_E immediately after a successful OPEN for larger
# uploads, always paired with a stale err=-1057 (WS_CHAN_RXD) -- heap
# diagnostics (heap_caps_get_free_size/heap_caps_get_largest_free_block)
# separately confirmed ~8.3MB free and ~8.25MB contiguous at the moment of
# failure, ruling out a genuine memory shortage.
#
# wolfSSH's own master branch (not yet in a numbered release as of this
# writing) fixes this same class of bug much more broadly via a shared
# NoticeError()/SFTP_CheckRecvSz() pair used at ~30 call sites throughout
# wolfsftp.c -- too large a divergence from our vendored 1.4.20 snapshot to
# port wholesale. This patches only the one call site that actually
# reproduces on this device (STATE_RECV_DO), using the same minimal,
# single-guard technique as patch_wolfssh_sftp_accept_chan_rxd.py, plus a
# defensive cap on the declared body length at STATE_RECV_READ (mirroring
# upstream's own SFTP_CheckRecvSz() bounds check) so a length ever again
# misparsed this way fails cleanly instead of attempting a wild WMALLOC.
#
# Invoked from micropython.cmake via execute_process(), same as the other
# patch scripts there. Plain text substitution rather than sed -- see
# patch_wolfssh_sftp_filesystem.py's header comment for why. Idempotent: each
# replace() is a no-op once its target text is already gone (already
# patched).

import sys

wolfssh_dir = sys.argv[1]
wolfsftp_c = f"{wolfssh_dir}/src/wolfsftp.c"

with open(wolfsftp_c, "r") as f:
    content = f.read()

# 1) Don't wipe recvState (and desync the read cursor) on WS_CHAN_RXD.
content = content.replace(
    "        case STATE_RECV_DO:\n"
    "            ret = wolfSSH_SFTP_buffer_read(ssh, &state->buffer,\n"
    "                    state->buffer.sz);\n"
    "            if (ret < 0) {\n"
    "                if (ssh->error != WS_WANT_READ && ssh->error != WS_WANT_WRITE)\n"
    "                        wolfSSH_SFTP_ClearState(ssh, STATE_ID_RECV);\n"
    "                return ret;\n"
    "            }\n",
    "        case STATE_RECV_DO:\n"
    "            ret = wolfSSH_SFTP_buffer_read(ssh, &state->buffer,\n"
    "                    state->buffer.sz);\n"
    "            if (ret < 0) {\n"
    "                if (ssh->error != WS_WANT_READ && ssh->error != WS_WANT_WRITE &&\n"
    "                        ssh->error != WS_CHAN_RXD)\n"
    "                        wolfSSH_SFTP_ClearState(ssh, STATE_ID_RECV);\n"
    "                return ret;\n"
    "            }\n",
    1,
)

# 2) Defensive cap on the declared body length, belt-and-suspenders against
# any other way a bogus/corrupted length could reach WMALLOC() -- mirrors
# upstream's later SFTP_CheckRecvSz() bound (declared length must be
# positive and no larger than a generous ceiling; the largest legitimate
# request this server ever receives is a WRITE carrying WOLFSSH_MAX_SFTP_RW
# (32768) bytes of file data plus framing, so 262144 leaves ample headroom
# without coming anywhere near "misparsed file content" territory).
content = content.replace(
    "            ret = SFTP_GetHeader(ssh, (word32*)&state->reqId,\n"
    "                    &state->type, &state->buffer);\n"
    "            if (ret <= 0) {\n"
    "                return WS_FATAL_ERROR;\n"
    "            }\n",
    "            ret = SFTP_GetHeader(ssh, (word32*)&state->reqId,\n"
    "                    &state->type, &state->buffer);\n"
    "            if (ret <= 0 || ret > 262144) {\n"
    "                return WS_FATAL_ERROR;\n"
    "            }\n",
    1,
)

with open(wolfsftp_c, "w") as f:
    f.write(content)
