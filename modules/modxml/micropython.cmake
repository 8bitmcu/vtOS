# Create an INTERFACE library for our C module.
add_library(usermod_modxml INTERFACE)

# Add our source files to the lib
target_sources(usermod_modxml INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/modxml.c
    ${CMAKE_CURRENT_LIST_DIR}/yxml.c)

# Add the current directory as an include directory.
target_include_directories(usermod_modxml INTERFACE
    ${CMAKE_CURRENT_LIST_DIR})

# Disable the "Stop on Warning"
target_compile_options(usermod_modxml INTERFACE -Wno-error=char-subscripts)

# Link our INTERFACE library to the usermod target.
target_link_libraries(usermod INTERFACE usermod_modxml)

