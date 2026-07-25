# Create an INTERFACE library for our C module.
add_library(usermod_modzm INTERFACE)

# The vendored frotz/blorb sources -- each one explicitly #includes
# modzm_compat.h itself (see the comment further down on why this isn't
# force-included via CMake instead). modzm_compat.h needs FreeRTOS/py
# headers that only resolve correctly when compiled as part of usermod's
# own component environment -- same reason this stays one INTERFACE
# library (inheriting that environment) rather than splitting out a
# separate STATIC library the way modvi did for neatvi (which has no
# such MicroPython/FreeRTOS entanglement of its own).
set(MODZM_FROTZ_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/frotz/blorb/blorblib.c
    ${CMAKE_CURRENT_LIST_DIR}/frotz/common/buffer.c
    ${CMAKE_CURRENT_LIST_DIR}/frotz/common/err.c
    ${CMAKE_CURRENT_LIST_DIR}/frotz/common/fastmem.c
    ${CMAKE_CURRENT_LIST_DIR}/frotz/common/files.c
    ${CMAKE_CURRENT_LIST_DIR}/frotz/common/getopt.c
    ${CMAKE_CURRENT_LIST_DIR}/frotz/common/hotkey.c
    ${CMAKE_CURRENT_LIST_DIR}/frotz/common/input.c
    ${CMAKE_CURRENT_LIST_DIR}/frotz/common/main.c
    ${CMAKE_CURRENT_LIST_DIR}/frotz/common/math.c
    ${CMAKE_CURRENT_LIST_DIR}/frotz/common/missing.c
    ${CMAKE_CURRENT_LIST_DIR}/frotz/common/object.c
    ${CMAKE_CURRENT_LIST_DIR}/frotz/common/process.c
    ${CMAKE_CURRENT_LIST_DIR}/frotz/common/quetzal.c
    ${CMAKE_CURRENT_LIST_DIR}/frotz/common/random.c
    ${CMAKE_CURRENT_LIST_DIR}/frotz/common/redirect.c
    ${CMAKE_CURRENT_LIST_DIR}/frotz/common/screen.c
    ${CMAKE_CURRENT_LIST_DIR}/frotz/common/sound.c
    ${CMAKE_CURRENT_LIST_DIR}/frotz/common/stream.c
    ${CMAKE_CURRENT_LIST_DIR}/frotz/common/table.c
    ${CMAKE_CURRENT_LIST_DIR}/frotz/common/text.c
    ${CMAKE_CURRENT_LIST_DIR}/frotz/common/variable.c
    ${CMAKE_CURRENT_LIST_DIR}/frotz/dumb/dblorb.c
    ${CMAKE_CURRENT_LIST_DIR}/frotz/dumb/dinit.c
    ${CMAKE_CURRENT_LIST_DIR}/frotz/dumb/dinput.c
    ${CMAKE_CURRENT_LIST_DIR}/frotz/dumb/doutput.c
    ${CMAKE_CURRENT_LIST_DIR}/frotz/dumb/dpic.c
)

# Add our source files to the lib
target_sources(usermod_modzm INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/modzm.c
    ${CMAKE_CURRENT_LIST_DIR}/modzm_compat.c
    ${MODZM_FROTZ_SOURCES})

# frotz has no central allocator wrapper (same situation as neatvi, see
# modules/modvi/modvi_compat.h) -- malloc/realloc/free are called
# directly across the vendored tree, including the undo mechanism's
# prev_zmp/undo_diff buffers (sized to the story's own dynamic memory)
# and blorb resource loading, none of it PSRAM-aware; modzm_compat.h
# also carries the VFS-aware stdio/character-I/O bridge every frotz
# source needs (formerly frotz_utils.h).
#
# Deliberately NOT force-included via any CMake mechanism -- each
# frotz/blorb source has its own explicit #include "../../modzm_compat.h"
# instead (see those files). Two different force-include approaches were
# tried and both failed for reasons specific to how this INTERFACE
# library's sources get folded into usermod: set_source_files_properties()
# silently didn't apply (modzm_fopen/fread/fclose/yield came back as
# "implicit declaration" once their explicit #include was removed), and
# target_compile_options(usermod_modzm INTERFACE -include ...) applied
# far too broadly -- INTERFACE options propagate to *everything* that
# transitively links against usermod, not just this library's own
# sources, and ended up conflicting with an unrelated C++ translation
# unit elsewhere in the firmware (C vs C++ linkage clash on
# modzm_malloc/realloc/free). A plain #include in each source sidesteps
# both failure modes entirely -- it's evaluated by the compiler like any
# other include, with no dependency on CMake's INTERFACE-library-into-
# usermod plumbing.

# Add the current directory as an include directory.
target_include_directories(usermod_modzm INTERFACE
    ${CMAKE_CURRENT_LIST_DIR})

# Disable the "Stop on Warning"
target_compile_options(usermod_modzm INTERFACE -Wno-error=char-subscripts)

# Link our INTERFACE library to the usermod target.
target_link_libraries(usermod INTERFACE usermod_modzm)
