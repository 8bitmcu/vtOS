# Include the vt module
include(${CMAKE_CURRENT_LIST_DIR}/vt/micropython.cmake)

# Include the display driver module
include(${CMAKE_CURRENT_LIST_DIR}/st7789/micropython.cmake)

# include the keyboard module
include(${CMAKE_CURRENT_LIST_DIR}/tdeck_kbd/micropython.cmake)

# include the keyboard/video module
include(${CMAKE_CURRENT_LIST_DIR}/tdeck_kvm/micropython.cmake)

# include the trackball module
include(${CMAKE_CURRENT_LIST_DIR}/tdeck_trk/micropython.cmake)

# include the zm module (Z-machine interpreter)
include(${CMAKE_CURRENT_LIST_DIR}/modzm/micropython.cmake)

# include the vttui (text ui) module
include(${CMAKE_CURRENT_LIST_DIR}/vttui/micropython.cmake)

# include the xml module (RSS Feeds)
include(${CMAKE_CURRENT_LIST_DIR}/xml/micropython.cmake)

# include the i2s module (audio)
include(${CMAKE_CURRENT_LIST_DIR}/tdeck_i2s/micropython.cmake)

# include the lora module
include(${CMAKE_CURRENT_LIST_DIR}/tdeck_lora/micropython.cmake)

# include the codec2 module (low-bitrate speech codec)
include(${CMAKE_CURRENT_LIST_DIR}/codec2/micropython.cmake)

# include the modssh module (wolfSSH-backed)
include(${CMAKE_CURRENT_LIST_DIR}/modssh/micropython.cmake)

# include the vi editor module
include(${CMAKE_CURRENT_LIST_DIR}/modvi/micropython.cmake)
