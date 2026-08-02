# Create an INTERFACE library for our C module.
add_library(usermod_modxml INTERFACE)

# Add our source files to the lib. mxml-index.c (mxmlIndex* API) isn't used
# by our binding -- omitted to keep the module smaller.
target_sources(usermod_modxml INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/modxml.c
    ${CMAKE_CURRENT_LIST_DIR}/modxml_compat.c
    ${CMAKE_CURRENT_LIST_DIR}/mxml/mxml-attr.c
    ${CMAKE_CURRENT_LIST_DIR}/mxml/mxml-file.c
    ${CMAKE_CURRENT_LIST_DIR}/mxml/mxml-get.c
    ${CMAKE_CURRENT_LIST_DIR}/mxml/mxml-node.c
    ${CMAKE_CURRENT_LIST_DIR}/mxml/mxml-options.c
    ${CMAKE_CURRENT_LIST_DIR}/mxml/mxml-private.c
    ${CMAKE_CURRENT_LIST_DIR}/mxml/mxml-search.c
    ${CMAKE_CURRENT_LIST_DIR}/mxml/mxml-set.c)

# Add the current directory (modxml.c's own headers) and the vendored
# mxml/ directory (config.h + mxml.h + mxml-private.h) as include dirs.
target_include_directories(usermod_modxml INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/mxml)

# Disable the "Stop on Warning"
target_compile_options(usermod_modxml INTERFACE -Wno-error=char-subscripts)

# Link our INTERFACE library to the usermod target.
target_link_libraries(usermod INTERFACE usermod_modxml)

