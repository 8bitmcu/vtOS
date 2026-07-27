USERMOD_DIR := $(USERMOD_DIR)

# Add our C file to the build
SRC_USERMOD_C += $(USERMOD_DIR)/modxml.c
SRC_USERMOD_C += $(USERMOD_DIR)/yxml.c

# Link it to the build system
CFLAGS_USERMOD += -I$(USERMOD_DIR)

# Disable the "Stop on Warning" behavior
CFLAGS_USERMOD += -Wno-error

