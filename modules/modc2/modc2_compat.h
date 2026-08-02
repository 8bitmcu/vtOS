/*
 * modc2_compat.h -- explicitly #included (first line, before any other
 * header -- see this file's own object-like-macro note below) into the
 * vendored codec2 source files that call bare malloc/calloc/free
 * themselves rather than going through debug_alloc.h's MALLOC()/
 * CALLOC()/FREE() macros. Copyright (c) 2026 8bitmcu. License: MIT
 *
 * modc2_alloc.c already provides codec2_malloc()/codec2_calloc()/
 * codec2_free() as PSRAM-preferring equivalents, and debug_alloc.h wires
 * them in via its MALLOC()/CALLOC()/FREE() macros -- but not every
 * vendored file uses that convention: mbest.c and nlp.c never include
 * debug_alloc.h at all, and codec2_fft.c/codec2.c include it but still
 * call malloc() directly at several sites (FFT twiddle/bit-reversal
 * tables, the NEWAMP1_K buffer, NLP's decimating-filter memory, mbest's
 * candidate lists) -- all of it firing during codec2_create()'s DSP init
 * burst, the same phase already flagged as heap-pressure-sensitive in
 * boards/LILYGO_T_DECK/mpconfigboard.h's MICROPY_TASK_STACK_SIZE comment.
 * Same class of fix already proven in this project for neatvi
 * (modules/modvi/modvi_compat.h) and frotz (modules/modzm/modzm_compat.h).
 *
 * Object-like macros (bare identifier, no parameter list), not
 * function-like ones -- see modvi_compat.h's own header comment for
 * exactly why function-like substitution breaks the moment a vendored
 * file's own #include <stdlib.h> reaches malloc/calloc/free's
 * *declarations*: the preprocessor doesn't know "this occurrence is a
 * declaration, not a call" and blindly substitutes there too. A bare
 * identifier substitution instead turns e.g. `void *malloc(size_t);`
 * into `void *codec2_malloc(size_t);` -- a harmless, type-compatible
 * re-declaration of the name this header already declared below.
 *
 * Explicit per-file #include rather than a CMake -include: modzm's own
 * micropython.cmake comment documents why -include on an INTERFACE
 * library (which is what usermod_modc2 is) leaks to everything that
 * transitively links against it, not just this library's own sources --
 * same failure mode, same fix (a plain #include in each source instead).
 */

#ifndef MODC2_COMPAT_H
#define MODC2_COMPAT_H

#include <stddef.h>

extern void *codec2_malloc(size_t n);
extern void *codec2_calloc(size_t nmemb, size_t n);
extern void codec2_free(void *p);

#define malloc codec2_malloc
#define calloc codec2_calloc
#define free codec2_free

#endif
