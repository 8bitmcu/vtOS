#!/usr/bin/env python3
# Raises DEFAULT_WINDOW_SZ back to wolfSSH's own upstream default (128KB,
# wolfssh/internal.h) from 2000 bytes -- the value wolfssl__wolfssl's
# vendored user_settings.h sets inside its `#if defined(ESP_ENABLE_WOLFSSH)
# || defined(CONFIG_ESP_ENABLE_WOLFSSH)` block, itself an Espressif-example
# default ("The default SSH Windows size is massive for an embedded
# target. Limit it:"), not something this project chose for its own SFTP
# use case.
#
# Confirmed as the root cause of a real device's multi-MB SFTP upload
# hangs: a temporary diagnostic accessor for the channel's receive window
# showed it cycling in the ~200-2000 byte range throughout a transfer --
# exactly matching this 2000-byte ceiling. A single SFTP WRITE payload
# (WOLFSSH_MAX_SFTP_RW, 32768 bytes) needs roughly 16 WINDOW_ADJUST
# round-trips just to get through at that window size; a multi-MB upload
# needs thousands of them. wolfSSH's own _UpdateChannelWindow() (src/
# ssh.c) only replenishes the window once its inputBuffer is more than
# half full or windowSz has already hit exactly 0, so at this size it's
# permanently living right at that edge -- any single missed/delayed
# WINDOW_ADJUST stalls the client's send indefinitely, and across
# thousands of cycles per transfer, hitting that is close to certain.
# Small/modest files simply don't do enough cycles to ever hit it, which
# is exactly the "small files fine, multi-MB hangs" split observed.
#
# 128KB was already comfortably affordable once this project's own
# WMALLOC() started routing through PSRAM (see
# patch_wolfssl_psram_alloc.py's successor, wolfssl_shim/psram_alloc.c +
# XMALLOC_USER) -- the channel inputBuffer this window size drives
# (ChannelNew(), src/internal.c) allocates through the same WMALLOC().
#
# Invoked from micropython.cmake via execute_process(), same as the other
# patch scripts there. Plain text substitution rather than sed -- see
# patch_wolfssh_sftp_filesystem.py's header comment for why. Idempotent:
# the replace() is a no-op once the target text is already gone (already
# patched).

import sys

wolfssl_dir = sys.argv[1]
user_settings_h = f"{wolfssl_dir}/include/user_settings.h"

with open(user_settings_h, "r") as f:
    content = f.read()

content = content.replace(
    "#define DEFAULT_WINDOW_SZ 2000\n",
    "#define DEFAULT_WINDOW_SZ (128 * 1024) "
    "/* vtOS: was 2000, too small for SFTP -- see patch_wolfssl_window_sz.py */\n",
    1,
)

with open(user_settings_h, "w") as f:
    f.write(content)
