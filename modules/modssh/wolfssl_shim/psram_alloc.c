/*
 * psram_alloc.c
 *
 * Real XMALLOC/XFREE/XREALLOC definitions for wolfSSL/wolfCrypt/wolfSSH,
 * enabled via XMALLOC_USER (see micropython.cmake). Needed because
 * wolfssl/wolfcrypt/settings.h's own FREERTOS/WOLFSSL_ESPIDF block
 * (confirmed directly against the vendored source, ~line 1487) *always*
 * defines these as pvPortMalloc()/vPortFree()/realloc() unless XMALLOC_USER
 * is set -- it doesn't check XMALLOC_OVERRIDE (that's a separate mechanism
 * checked later, in wolfcrypt/types.h, which settings.h's own definitions
 * already pre-empt by the time types.h would look). With XMALLOC_USER set,
 * settings.h steps aside entirely and types.h instead declares these three
 * as plain extern functions -- which this file provides.
 *
 * Confirmed necessary, not theoretical: pvPortMalloc() on this board only
 * ever draws from internal SRAM, which sits at roughly 22-42KB free/largest-
 * contiguous-block under normal SFTP session load (measured directly on a
 * real device). A single incoming SFTP WRITE message (~32KB, matching
 * WOLFSSH_MAX_SFTP_RW) needs one contiguous allocation
 * larger than that, so it reliably fails with WS_MEMORY_E -- even though
 * this board has 8MB of PSRAM sitting idle right next to it, unreachable
 * because nothing routes allocations there. Same PSRAM-preferring,
 * internal-RAM-or-PSRAM-fallback pattern already used by this codebase's
 * other heap-hungry consumers (modules/tdeck_i2s/ring_buf.h, modules/
 * modc2/modc2_alloc.c).
 *
 * Real (non-static, non-inline) function definitions, not macros -- these
 * get linked into wolfssl_lib (see micropython.cmake's target_sources())
 * and referenced via extern declaration from every wolfSSL/wolfCrypt/
 * wolfSSH translation unit, including wolfssh_lib's.
 */

#include <stddef.h>

#include "esp_heap_caps.h"

void *XMALLOC(size_t n, void *heap, int type) {
  (void)heap;
  (void)type;
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p == NULL) {
    p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
  }
  return p;
}

void *XREALLOC(void *p, size_t n, void *heap, int type) {
  (void)heap;
  (void)type;
  void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (q == NULL) {
    q = heap_caps_realloc(p, n, MALLOC_CAP_8BIT);
  }
  return q;
}

void XFREE(void *p, void *heap, int type) {
  (void)heap;
  (void)type;
  heap_caps_free(p);
}
