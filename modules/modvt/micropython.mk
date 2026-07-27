TERM_MOD_DIR := $(USERMOD_DIR)

# Add all C files to SRC_USERMOD.
SRC_USERMOD += $(TERM_MOD_DIR)/modvt.c
SRC_USERMOD += $(TERM_MOD_DIR)/st.c
SRC_USERMOD += $(TERM_MOD_DIR)/fb.c

# We can add our module folder to include paths if needed
# This is not actually needed in this example.
CFLAGS_USERMOD += -I$(TERM_MOD_DIR)
