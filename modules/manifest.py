import os

# Read the path passed by the build system.
# Fall back to your local path if the environment variable isn't found.

mpy_dir = os.getenv("MPY_USER_DIR", "/opt/micropython")
include(mpy_dir + "/extmod/asyncio/manifest.py")

base_path = os.getenv("MPY_USER_MODULES_DIR", "/opt/all_modules")

# scripts/bin, scripts/fonts and scripts/usb are real packages (each has
# its own __init__.py), frozen with their directory name preserved as a
# prefix -- e.g. bin/wiki.py -> "bin.wiki", matching every `_app("bin.x")`
# / `import bin.x` call site.
freeze(base_path + "/scripts", "boot.py")
freeze(base_path + "/scripts", "bin")
freeze(base_path + "/scripts", "fonts")
freeze(base_path + "/scripts", "usb")

# scripts/lib and scripts/sys are NOT packages -- pure source-tree
# groupings (library helpers vs. core boot/runtime files), no
# __init__.py. Each gets its own freeze() call using the subdirectory
# itself as the freeze path, so freeze()'s "the name of the module
# starts after `path`" rule flattens their contents to plain top-level
# module names (lib/hardware.py -> "hardware", not "sys.hardware") --
# matching every existing bare `import hardware` / `import epubzip`
# call site unchanged. This also sidesteps a real landmine: "sys"
# collides head-on with the built-in `sys` module (imported by nearly
# every app here for sys.stdin) -- freezing it as a dotted "sys.*"
# package would either be permanently unreachable behind the built-in
# or corrupt its registration outright. Keeping scripts/sys/* out of
# any dotted namespace avoids that entirely.
freeze(base_path + "/scripts/lib")
freeze(base_path + "/scripts/sys")
