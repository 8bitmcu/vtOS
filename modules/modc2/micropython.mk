USERMOD_DIR := $(USERMOD_DIR)
# Vendored Codec2 source (David Rowe, LGPL-2.1 -- see modules/modc2/COPYING),
# mirrored from https://github.com/drowe67/codec2 (src/).
SRC_USERMOD += $(wildcard $(USERMOD_DIR)/vendor/*.c)
SRC_USERMOD += $(USERMOD_DIR)/modc2.c
SRC_USERMOD += $(USERMOD_DIR)/modc2_alloc.c
# Link it to the build system
CFLAGS_USERMOD += -I$(USERMOD_DIR) -I$(USERMOD_DIR)/vendor
CFLAGS_USERMOD += -D__EMBEDDED__
