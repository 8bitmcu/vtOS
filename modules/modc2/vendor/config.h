/*--------------------------------------------------------------------------
 ** Normally generated from config.h.in by CMake's feature-detection checks
 ** at configure time. Hand-written here since this project vendors
 ** codec2's source directly rather than running its CMake build. Values
 ** reflect what's actually available in ESP-IDF's newlib -- all the
 ** listed libc/libm functions are present; HAVE_GETOPT is intentionally
 ** left undefined (getopt() isn't part of ESP-IDF's newlib, and nothing
 ** in the vendored core codec sources -- as opposed to codec2's own
 ** command-line tools, which aren't vendored here -- needs it).
 ** --------------------------------------------------------------------------*/
#ifndef _CONFIGURATION_HEADER_GUARD_H_
#define _CONFIGURATION_HEADER_GUARD_H_

#define SIZEOF_INT 4
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_FLOOR 1
#define HAVE_CEIL 1
#define HAVE_MEMSET 1
#define HAVE_POW 1
#define HAVE_SQRT 1
#define HAVE_SIN 1
#define HAVE_COS 1
#define HAVE_ATAN2 1
#define HAVE_LOG10 1
#define HAVE_ROUND 1

#endif
