//
// Hand-written config.h for vendoring Mini-XML v4.0.4 into vtOS's ESP-IDF
// build. Upstream normally generates this via `./configure`, which isn't
// available/desired inside a MicroPython usermod CMake build -- this is
// what configure would produce on a modern C99 GCC target: pthreads
// disabled (MicroPython C modules run single-threaded here) and no
// `inline` macro override (ESP-IDF's toolchain has native C99 inline, so
// leaving it undefined lets the keyword pass through as-is, same as
// configure would do on any GCC host).
//
#ifndef MXML_CONFIG_H
#  define MXML_CONFIG_H
#  include <stdio.h>
#  include <stdlib.h>
#  include <string.h>
#  include <stdarg.h>
#  include <ctype.h>

#  define MXML_VERSION "4.0.4"

#  undef HAVE_PTHREAD_H

#endif // !MXML_CONFIG_H
