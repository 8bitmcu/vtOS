# neatvi (https://github.com/aligrudi/neatvi), vendored under neatvi/ and
# adapted to run as a MicroPython C module -- see this port's plan and
# modvi.c's header comment for the full architecture. Modeled
# directly on modules/vi/'s own integration of Toybox's vi (same VFS-
# bridge/KVM-stream-input/PSRAM-allocator problems, same solutions).

# neatvi has no central allocator wrapper (unlike modules/vi/vi.c's
# vi_xmalloc/vi_xrealloc) -- malloc/realloc/free are called directly
# across the whole vendored tree. Rather than editing ~80-120 call
# sites, this library force-includes modvi_compat.h (see that file's
# own header comment for the full rationale) into just these files via
# a target-scoped -include, redirecting them to PSRAM-preferring
# equivalents -- the same class of fix already proven this project for
# wolfSSL's own internal-SRAM-only allocator trap (see modules/modssh/
# wolfssl_shim/psram_alloc.c). A separate library (rather than
# set_source_files_properties() on individual files) keeps this scoped
# via ordinary target-level properties, matching how this codebase
# already scopes other per-compilation-unit settings (modules/modssh/
# micropython.cmake's wolfssl_lib/wolfssh_lib are likewise separate
# targets from their sibling usermod_modssh, for the same kind of
# reason). STATIC, not OBJECT: confirmed the hard way that ESP-IDF's
# own linker-generation tooling (tools/cmake/ldgen.cmake) walks every
# linked target's $<TARGET_FILE:...> and fails outright for OBJECT
# libraries, which don't produce one (no single archive/executable
# output) -- a STATIC library does, and is otherwise a drop-in
# replacement here (target_compile_options/target_include_directories
# work identically either way).
add_library(usermod_modvi_neatvi_core STATIC
    ${CMAKE_CURRENT_LIST_DIR}/neatvi/cmd.c
    ${CMAKE_CURRENT_LIST_DIR}/neatvi/conf.c
    ${CMAKE_CURRENT_LIST_DIR}/neatvi/dir.c
    ${CMAKE_CURRENT_LIST_DIR}/neatvi/ex.c
    ${CMAKE_CURRENT_LIST_DIR}/neatvi/lbuf.c
    ${CMAKE_CURRENT_LIST_DIR}/neatvi/led.c
    ${CMAKE_CURRENT_LIST_DIR}/neatvi/mot.c
    ${CMAKE_CURRENT_LIST_DIR}/neatvi/reg.c
    ${CMAKE_CURRENT_LIST_DIR}/neatvi/regex.c
    ${CMAKE_CURRENT_LIST_DIR}/neatvi/ren.c
    ${CMAKE_CURRENT_LIST_DIR}/neatvi/rset.c
    ${CMAKE_CURRENT_LIST_DIR}/neatvi/rstr.c
    ${CMAKE_CURRENT_LIST_DIR}/neatvi/sbuf.c
    ${CMAKE_CURRENT_LIST_DIR}/neatvi/syn.c
    ${CMAKE_CURRENT_LIST_DIR}/neatvi/tag.c
    ${CMAKE_CURRENT_LIST_DIR}/neatvi/uc.c
    ${CMAKE_CURRENT_LIST_DIR}/neatvi/vi.c
)
target_include_directories(usermod_modvi_neatvi_core PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/neatvi
)
target_compile_options(usermod_modvi_neatvi_core PRIVATE
    -include ${CMAKE_CURRENT_LIST_DIR}/modvi_compat.h
)

# usermod_modvi_neatvi_core is a plain STATIC library, not a registered
# ESP-IDF component -- unlike modvi_compat.c (compiled as part of the
# usermod_modvi INTERFACE library, which inherits usermod's own component
# include paths), it doesn't automatically see ESP-IDF headers like
# esp_attr.h (needed by EXT_RAM_BSS_ATTR, see e.g. neatvi/vi.c). Same
# fix already proven in this project for wolfssl_lib/wolfssh_lib above:
# look up the real component library/include dir and link against it.
idf_component_get_property(esp_common_dir esp_common COMPONENT_DIR)
idf_component_get_property(esp_common_lib esp_common COMPONENT_LIB)
target_include_directories(usermod_modvi_neatvi_core PRIVATE
    ${esp_common_dir}/include
)
target_link_libraries(usermod_modvi_neatvi_core PRIVATE ${esp_common_lib})

# Create an INTERFACE library for our C module -- the non-vendored,
# MicroPython-aware glue: the module wrapper, the terminal-I/O
# replacement for neatvi's own term.c, the VFS file-I/O bridge, and the
# allocator substitution's real implementation. None of these call bare
# malloc/realloc/free, so they must NOT get modvi_compat.h's
# substitution -- that's why they're a separate target from the STATIC
# library above rather than sharing its compile options.
add_library(usermod_modvi INTERFACE)
target_sources(usermod_modvi INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/modvi.c
    ${CMAKE_CURRENT_LIST_DIR}/modvi_term.c
    ${CMAKE_CURRENT_LIST_DIR}/modvi_vfs.c
    ${CMAKE_CURRENT_LIST_DIR}/modvi_compat.c
)

# Both this directory (modvi.h/modvi_vfs.h/modvi_compat.h) and
# neatvi/ (vi.h, #include "vi.h"'d by every vendored file via a plain ""
# include) need to be on the include path.
target_include_directories(usermod_modvi INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/neatvi
)

# Pull the vendored core into this INTERFACE library (and so into
# whatever finally links against it) -- an INTERFACE library linking
# against a STATIC library is standard, well-supported CMake usage.
target_link_libraries(usermod_modvi INTERFACE usermod_modvi_neatvi_core)

# Link our INTERFACE library to the usermod target.
target_link_libraries(usermod INTERFACE usermod_modvi)
