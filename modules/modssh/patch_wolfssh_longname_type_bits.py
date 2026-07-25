#!/usr/bin/env python3
# Patches src/wolfsftp.c's SFTP_CreateLongName(): its file-type check for
# the permission-string prefix ('d'/'l'/'-') tests raw bit overlap
# (`tmp & FILEATRB_PER_DIR`, `tmp & FILEATRB_PER_LINK`) instead of masking
# to the type field first and comparing exactly. FILEATRB_PER_LINK
# (0120000) and FILEATRB_PER_FILE (0100000) share a bit (0120000 =
# 0100000 | 0020000), so a plain regular file (mode 0100000, exactly what
# MicroPython's os.stat() returns for a file with no extra permission
# bits) spuriously matches `tmp & FILEATRB_PER_LINK` and gets shown as a
# symlink ('l-------') in the server-built longname string -- confirmed
# against a real device: os.stat('/flash/WELCOME.md')[0] == 0o100000,
# and FileZilla (which parses the SFTP-v3 longname field for its
# permission-column display, standard client behavior for this protocol
# version) showed every plain file as a symlink. FILEATRB_PER_DIR
# (0040000) doesn't have this exact overlap with FILE, but the same
# imprecise-test pattern is still wrong in general, so both branches are
# fixed to mask against FILEATRB_PER_MASK_TYPE (0770000) and compare
# exactly, matching how POSIX's own S_ISDIR/S_ISLNK macros test S_IFMT.
#
# This is a genuine upstream wolfSSH bug (in src/wolfsftp.c's own
# SFTP_CreateLongName(), not anything in modsftpd.c/myFilesystem.h),
# same class of "local patch to the vendored 1.4.20 snapshot" as
# patch_wolfssh_sftp_accept_chan_rxd.py.
#
# Invoked from micropython.cmake via execute_process(), same as the
# other patch scripts there. Plain text substitution rather than sed --
# see patch_wolfssh_sftp_filesystem.py's header comment for why (fragile
# through CMake's own string-escaping layer). Idempotent: the replace()
# is a no-op once the target text is already gone (already patched).

import sys

wolfssh_dir = sys.argv[1]
wolfsftp_c = f"{wolfssh_dir}/src/wolfsftp.c"

with open(wolfsftp_c, "r") as f:
    content = f.read()

content = content.replace(
    "        if (tmp & FILEATRB_PER_DIR) {\n"
    "            perm[i++] = 'd';\n"
    "        }\n"
    "        else if (tmp & FILEATRB_PER_LINK) {\n"
    "            perm[i++] = 'l';\n"
    "        }\n",
    "        if ((tmp & FILEATRB_PER_MASK_TYPE) == FILEATRB_PER_DIR) {\n"
    "            perm[i++] = 'd';\n"
    "        }\n"
    "        else if ((tmp & FILEATRB_PER_MASK_TYPE) == FILEATRB_PER_LINK) {\n"
    "            perm[i++] = 'l';\n"
    "        }\n",
    1,
)

with open(wolfsftp_c, "w") as f:
    f.write(content)
