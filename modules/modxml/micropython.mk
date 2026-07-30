USERMOD_DIR := $(USERMOD_DIR)

# Add our C files to the build
SRC_USERMOD_C += $(USERMOD_DIR)/modxml.c
SRC_USERMOD_C += $(USERMOD_DIR)/mxml/mxml-attr.c
SRC_USERMOD_C += $(USERMOD_DIR)/mxml/mxml-file.c
SRC_USERMOD_C += $(USERMOD_DIR)/mxml/mxml-get.c
SRC_USERMOD_C += $(USERMOD_DIR)/mxml/mxml-node.c
SRC_USERMOD_C += $(USERMOD_DIR)/mxml/mxml-options.c
SRC_USERMOD_C += $(USERMOD_DIR)/mxml/mxml-private.c
SRC_USERMOD_C += $(USERMOD_DIR)/mxml/mxml-search.c
SRC_USERMOD_C += $(USERMOD_DIR)/mxml/mxml-set.c

# Link it to the build system
CFLAGS_USERMOD += -I$(USERMOD_DIR)
CFLAGS_USERMOD += -I$(USERMOD_DIR)/mxml

# Disable the "Stop on Warning" behavior
CFLAGS_USERMOD += -Wno-error

