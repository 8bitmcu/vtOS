# Add our new C module
add_library(usermod_modc2 INTERFACE)

# Vendored Codec2 source (David Rowe, LGPL-2.1 -- see modules/modc2/COPYING).
# Globbed rather than listed by hand: this is third-party source we don't
# edit, mirrored from https://github.com/drowe67/codec2 (src/), matching
# upstream's own CODEC2_SRCS file list from src/CMakeLists.txt.
file(GLOB CODEC2_VENDOR_SRCS ${CMAKE_CURRENT_LIST_DIR}/vendor/*.c)

target_sources(usermod_modc2 INTERFACE
  ${CODEC2_VENDOR_SRCS}
  ${CMAKE_CURRENT_LIST_DIR}/modc2.c
  ${CMAKE_CURRENT_LIST_DIR}/modc2_alloc.c
)
target_include_directories(usermod_modc2 INTERFACE
  ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/vendor
)

target_compile_definitions(usermod_modc2 INTERFACE __EMBEDDED__)

# Register the module with MicroPython
target_link_libraries(usermod INTERFACE usermod_modc2)
