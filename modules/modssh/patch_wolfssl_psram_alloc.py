#!/usr/bin/env python3
# Cleanup for an earlier, broken approach to the same problem this project
# now solves via XMALLOC_USER + wolfssl_shim/psram_alloc.c (see
# micropython.cmake for the current mechanism and full rationale). An
# earlier version of this script appended a `#define XMALLOC_OVERRIDE` block
# with static-inline allocator functions directly to user_settings.h --
# confirmed broken two ways on a real build: (1) wolfssl/wolfcrypt/
# settings.h's own FREERTOS/WOLFSSL_ESPIDF block only steps aside for
# XMALLOC_USER, not XMALLOC_OVERRIDE, so it silently redefined XMALLOC/
# XFREE/XREALLOC right back to pvPortMalloc()/vPortFree()/realloc()
# afterward, and (2) user_settings.h gets processed more than once within a
# single translation unit in this include chain, and unlike a plain #define
# (which just warns on redefinition), the static inline *function*
# definitions caused a hard "redefinition of 'vtos_wolfssl_malloc'" compile
# error.
#
# Since the wolfssl__wolfssl component directory persists across builds
# (idf_component_manager caches the fetched/extracted source rather than
# re-fetching it every time), simply stopping this script from *adding* the
# broken block wouldn't remove what a previous build already appended --
# this strips it back out instead, so a normal rebuild self-heals regardless
# of whether the component cache is cleared. Safe to run against a fresh
# fetch that never had the block appended (no-op).
#
# Invoked from micropython.cmake via execute_process(), same as the other
# patch scripts there.

import sys

wolfssl_dir = sys.argv[1]
user_settings_h = f"{wolfssl_dir}/include/user_settings.h"

SENTINEL = "/* vtOS: PSRAM-preferring XMALLOC_OVERRIDE */"

with open(user_settings_h, "r") as f:
    content = f.read()

idx = content.find(SENTINEL)
if idx != -1:
    content = content[:idx].rstrip("\n") + "\n"
    with open(user_settings_h, "w") as f:
        f.write(content)
