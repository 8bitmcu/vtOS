/*
 * modxml_compat.c -- PSRAM-preferring malloc/calloc/realloc/free
 * implementations backing modxml_compat.h's macro substitution.
 * Copyright (c) 2026 8bitmcu. License: MIT
 *
 * Same pattern already proven this project for the same class of
 * problem: modules/modvi/modvi_compat.c, modules/modzm/modzm_compat.c,
 * modules/modc2/modc2_alloc.c, modules/modssh/wolfssl_shim/psram_alloc.c.
 */

// Included for prototype-consistency only -- the #define malloc/calloc/
// realloc/free substitution it also carries has no effect here, since
// these functions call heap_caps_malloc/_calloc/_realloc/_free
// directly, never bare malloc/calloc/realloc/free.
#include "modxml_compat.h"
#include "esp_heap_caps.h"

void *modxml_malloc(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p == NULL) {
    p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
  }
  return p;
}

void *modxml_calloc(size_t nmemb, size_t n) {
  void *p = heap_caps_calloc(nmemb, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p == NULL) {
    p = heap_caps_calloc(nmemb, n, MALLOC_CAP_8BIT);
  }
  return p;
}

void *modxml_realloc(void *p, size_t n) {
  void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (q == NULL) {
    q = heap_caps_realloc(p, n, MALLOC_CAP_8BIT);
  }
  return q;
}

void modxml_free(void *p) { heap_caps_free(p); }
