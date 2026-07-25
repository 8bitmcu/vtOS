# Create an INTERFACE library for our C module.
add_library(usermod_modzm INTERFACE)

# The vendored frotz/blorb sources -- these get modzm_compat.h's malloc/
# realloc/free -> PSRAM-preferring substitution (and its merged-in VFS/
# terminal-I/O bridge, formerly frotz_utils.h) below. modzm.c is
# deliberately NOT in this list: it uses MicroPython's own m_new_obj/
# stream APIs directly. modzm_compat.h itself needs FreeRTOS/py headers
# that only resolve correctly when compiled as part of usermod's own
# component environment -- same reason this stays one INTERFACE library
# (inheriting that environment) rather than splitting out a separate
# STATIC library the way modvi did for neatvi (which has no such
# MicroPython/FreeRTOS entanglement of its own).
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
# and blorb resource loading, none of it PSRAM-aware. Force-include
# modzm_compat.h into just these files, scoped via
# set_source_files_properties rather than a separate target (see
# MODZM_FROTZ_SOURCES's comment above for why a separate STATIC library
# isn't a clean fit here).
set_source_files_properties(${MODZM_FROTZ_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/modzm_compat.h")

# Add the current directory as an include directory.
target_include_directories(usermod_modzm INTERFACE
    ${CMAKE_CURRENT_LIST_DIR})

# Disable the "Stop on Warning"
target_compile_options(usermod_modzm INTERFACE -Wno-error=char-subscripts)

# Link our INTERFACE library to the usermod target.
target_link_libraries(usermod INTERFACE usermod_modzm)
