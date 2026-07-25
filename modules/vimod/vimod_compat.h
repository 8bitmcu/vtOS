/*
 * vimod_compat.h -- force-included (via -include, see micropython.cmake)
 * into neatvi's own vendored .c files only, redirecting malloc/realloc/
 * free to PSRAM-preferring equivalents. Copyright (c) 2026 8bitmcu.
 * License: MIT
 *
 * neatvi has no central allocator wrapper (unlike modules/vi/vi.c's
 * vi_xmalloc/vi_xrealloc) -- malloc/realloc/free are called directly
 * across ~80-120 sites scattered through the whole tree. Rather than
 * editing every call site, this macro-substitutes them at the
 * preprocessor level, scoped to only the vendored neatvi\*.c
 * translation units (never vimod_module.c/vimod_term.c/vimod_vfs.c,
 * which use MicroPython's own m_new/m_malloc APIs, not bare malloc).
 *
 * Routes through heap_caps_malloc()/_realloc()/_free() rather than
 * MicroPython's GC-tracked m_malloc/m_renew family: heap_caps_realloc()
 * has a real POSIX-compatible signature (the allocator tracks the
 * original size internally, same as libc's own realloc()), while
 * m_renew_maybe() requires the *caller* to already know and pass the
 * old size (confirmed directly in modules/vi/vi.c's own vi_xrealloc(),
 * which takes old_size as an explicit parameter for exactly this
 * reason) -- a mismatch with neatvi's libc-style call sites, which
 * don't track sizes themselves. See this port's plan for the resulting
 * tradeoff: neatvi's own allocations are real and untracked by
 * MicroPython's GC, so if an exception unwinds mid-session, whatever
 * was allocated up to that point leaks rather than being reclaimed --
 * bounded (one file's worth of state) and expected to be rare.
 *
 * Object-like macros (bare identifier, no parameter list), not
 * function-like ones -- confirmed the hard way that function-like
 * macros (`#define realloc(p, n) vimod_realloc((p), (n))`) break the
 * moment any vendored file's own #include <stdlib.h> reaches realloc's
 * *declaration*: the preprocessor doesn't know "this occurrence is a
 * declaration, not a call" and blindly substitutes there too, turning
 * `void *realloc(void *ptr, size_t size);` into the nonsensical `void
 * *vimod_realloc((ptr), (size));`. A bare identifier substitution
 * instead turns that same declaration into `void *vimod_realloc(void
 * *ptr, size_t size);` -- a harmless, type-compatible re-declaration of
 * the name this header already declared above, since the parameter
 * types match exactly (only the parameter *names* differ, which C
 * declarations don't care about).
 */

#ifndef VIMOD_COMPAT_H
#define VIMOD_COMPAT_H

#include <stddef.h>

void *vimod_malloc(size_t n);
void *vimod_realloc(void *p, size_t n);
void vimod_free(void *p);

#define malloc vimod_malloc
#define realloc vimod_realloc
#define free vimod_free

#endif
