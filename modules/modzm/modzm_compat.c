/*
 * modzm_compat.c -- PSRAM-preferring malloc/realloc/free implementations
 * backing modzm_compat.h's macro substitution. Copyright (c) 2026
 * 8bitmcu. License: MIT
 *
 * Same pattern already proven this project for the same class of
 * problem: modules/modssh/wolfssl_shim/psram_alloc.c and
 * modules/modvi/modvi_compat.c.
 */

// Included for prototype-consistency only -- the #define malloc/realloc/
// free substitution it also carries has no effect here, since these
// functions call heap_caps_malloc/_realloc/_free directly, never bare
// malloc/realloc/free.
#include "modzm_compat.h"
#include "esp_heap_caps.h"

void *modzm_malloc(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p == NULL) {
    p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
  }
  return p;
}

void *modzm_realloc(void *p, size_t n) {
  void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (q == NULL) {
    q = heap_caps_realloc(p, n, MALLOC_CAP_8BIT);
  }
  return q;
}

void modzm_free(void *p) { heap_caps_free(p); }
