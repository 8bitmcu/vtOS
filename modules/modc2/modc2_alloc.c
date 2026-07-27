/*
 * modc2_alloc.c
 *
 * codec2_malloc()/codec2_calloc()/codec2_free(): with __EMBEDDED__ defined
 * (see micropython.cmake), vendor/debug_alloc.h declares these as extern
 * rather than aliasing them to plain malloc()/calloc()/free() -- codec2
 * expects the embedding project to provide them, so allocations can be
 * routed through whatever allocator suits the target.
 *
 * Preferring PSRAM here (same pattern as audiorecorder.c/audioplayer.c's
 * own buffers) keeps a struct CODEC2's working state off internal SRAM,
 * which is already under real pressure on this board -- falls back to
 * internal RAM only if PSRAM is unavailable or exhausted.
 */

#include <stddef.h>

#include "esp_heap_caps.h"

void *codec2_malloc(size_t size) {
  return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void *codec2_calloc(size_t nmemb, size_t size) {
  return heap_caps_calloc(nmemb, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void codec2_free(void *ptr) { heap_caps_free(ptr); }
