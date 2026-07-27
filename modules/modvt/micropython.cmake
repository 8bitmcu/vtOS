# Define the module and its source files
add_library(usermod_modvt INTERFACE)

target_sources(usermod_modvt INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/st.c
    ${CMAKE_CURRENT_LIST_DIR}/fb.c
    ${CMAKE_CURRENT_LIST_DIR}/modvt.c
)

# Add the include directory so headers are found
target_include_directories(usermod_modvt INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
		${CMAKE_CURRENT_LIST_DIR}/../st7789
)

# Link it to the usermod target
target_link_libraries(usermod INTERFACE usermod_modvt)
