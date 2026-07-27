
# Create an INTERFACE library for our C module.
add_library(usermod_modtui INTERFACE)

# Add our source files to the lib
target_sources(usermod_modtui INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/modtui.c)

# Add the current directory as an include directory.
target_include_directories(usermod_modtui INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../modvt
)

# Link our INTERFACE library to the usermod target.
target_link_libraries(usermod INTERFACE usermod_modtui)


