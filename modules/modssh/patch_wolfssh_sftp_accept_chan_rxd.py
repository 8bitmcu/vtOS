#!/usr/bin/env python3
# Patches src/wolfsftp.c's wolfSSH_SFTP_accept(): its SFTP_BEGIN/SFTP_EXT
# case wipes all SFTP receive state (wolfSSH_SFTP_ClearState(ssh,
# STATE_ID_ALL)) on any non-success return from SFTP_ServerRecvInit()
# except WS_WANT_READ/WS_WANT_WRITE -- WS_CHAN_RXD ("channel data
# received", a normal transient status per wolfssh/error.h, not a fault)
# isn't exempted. Confirmed against a real SFTP client: its INIT packet
# reliably triggers WS_CHAN_RXD at least once, and every time it does,
# this clears state and restarts SFTP_ServerRecvInit() from scratch --
# forever, since the restart hits the exact same WS_CHAN_RXD again before
# ever accumulating a complete packet. Server never progresses past the
# handshake; wolfSSH_SFTP_read()'s own get_error() correctly reports
# WS_CHAN_RXD each time, but retrying (as its own vocabulary implies is
# correct) just repeats the same wipe-and-restart cycle.
#
# This is the server-side counterpart of a bug wolfSSH's own v1.4.21
# fixed on the *client* side (SFTP_ClientRecvInit(), upstream PR 846,
# "SFTP fix for init to handle channel data") -- our vendored 1.4.20
# predates that fix, and it was never applied to this server-side
# function (a much less exercised code path than the client one).
#
# Invoked from micropython.cmake via execute_process(), same as the
# other patch scripts there. Plain text substitution rather than sed --
# the replacement text itself contains && characters, which sed's s///
# replacement syntax would misinterpret as "insert matched text" (& is
# a replacement metacharacter there); Python string literals sidestep
# that ambiguity entirely, same rationale as
# patch_wolfssh_sftp_filesystem.py. Idempotent: the replace() is a no-op
# once the target text is already gone (already patched).

import sys

wolfssh_dir = sys.argv[1]
wolfsftp_c = f"{wolfssh_dir}/src/wolfsftp.c"

with open(wolfsftp_c, "r") as f:
    content = f.read()

content = content.replace(
    "                if (ssh->error != WS_WANT_READ && ssh->error != WS_WANT_WRITE)\n"
    "                    wolfSSH_SFTP_ClearState(ssh, STATE_ID_ALL);\n",
    "                if (ssh->error != WS_WANT_READ && ssh->error != WS_WANT_WRITE &&\n"
    "                        ssh->error != WS_CHAN_RXD)\n"
    "                    wolfSSH_SFTP_ClearState(ssh, STATE_ID_ALL);\n",
    1,
)

with open(wolfsftp_c, "w") as f:
    f.write(content)
