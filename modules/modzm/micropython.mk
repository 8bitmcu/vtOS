USERMOD_DIR := $(USERMOD_DIR)

# Add our C file to the build
SRC_USERMOD_C += $(USERMOD_DIR)/modzm.c
SRC_USERMOD_C += $(USERMOD_DIR)/modzm_compat.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/blorb/blorblib.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/common/buffer.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/common/err.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/common/fastmem.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/common/files.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/common/getopt.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/common/hotkey.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/common/input.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/common/main.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/common/math.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/common/missing.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/common/object.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/common/process.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/common/quetzal.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/common/random.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/common/redirect.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/common/screen.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/common/sound.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/common/stream.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/common/table.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/common/text.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/common/variable.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/dumb/dblorb.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/dumb/dinit.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/dumb/dinput.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/dumb/doutput.c
SRC_USERMOD_C += $(USERMOD_DIR)/frotz/dumb/dpic.c

# Link it to the build system
CFLAGS_USERMOD += -I$(USERMOD_DIR)
CFLAGS_USERMOD += -I$(USERMOD_DIR)/frotz/src
CFLAGS_USERMOD += -I$(USERMOD_DIR)/frotz/common
CFLAGS_USERMOD += -I$(USERMOD_DIR)/frotz/dumb

# Disable the "Stop on Warning" behavior
CFLAGS_USERMOD += -Wno-error
