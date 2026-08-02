/*
 * modxml_compat.h -- explicitly #included (first line, before any other
 * header) into every vendored mxml source file, redirecting malloc/
 * calloc/realloc/free to PSRAM-preferring equivalents. Copyright (c)
 * 2026 8bitmcu. License: MIT
 *
 * mxml has no central allocator wrapper -- malloc/calloc/realloc/free
 * are called directly across the vendored tree (the whole parse tree
 * built by xml_parse() in modxml.c is plain malloc()'d memory, per that
 * function's own comment on why it must be freed explicitly rather than
 * left to a Python finalizer). Unlike every other vendored library in
 * this project (neatvi: modules/modvi/modvi_compat.h, frotz: modules/
 * modzm/modzm_compat.h, codec2: modules/modc2/modc2_compat.h, wolfSSL:
 * modules/modssh/wolfssl_shim/psram_alloc.c), mxml never got this
 * treatment -- this closes that gap with the same proven pattern.
 *
 * Object-like macros (bare identifier, no parameter list), not
 * function-like ones -- see modvi_compat.h's own header comment for
 * exactly why function-like substitution breaks the moment a vendored
 * file's own #include <stdlib.h> reaches malloc/calloc/realloc/free's
 * *declarations*.
 *
 * Explicit per-file #include rather than a CMake -include: modzm's own
 * micropython.cmake comment documents why -include on an INTERFACE
 * library (which is what usermod_modxml is) leaks to everything that
 * transitively links against it, not just this library's own sources.
 */

#ifndef MODXML_COMPAT_H
#define MODXML_COMPAT_H

#include <stddef.h>

void *modxml_malloc(size_t n);
void *modxml_calloc(size_t nmemb, size_t n);
void *modxml_realloc(void *p, size_t n);
void modxml_free(void *p);

#define malloc modxml_malloc
#define calloc modxml_calloc
#define realloc modxml_realloc
#define free modxml_free

#endif
