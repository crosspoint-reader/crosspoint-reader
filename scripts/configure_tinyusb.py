"""Include the USB overlay contents in every affected compiler command's cache key."""

import hashlib
from pathlib import Path

Import("env")  # noqa: F821 -- PlatformIO

config = Path(env.subst("$PROJECT_DIR")) / "scripts/tinyusb/crosspoint_tusb_config.h"
# SCons' C scanner does not follow the CFG_TUSB_CONFIG_FILE macro include.
# Compiler flags reach both the Arduino core and the separately built component.
digest = hashlib.sha256(config.read_bytes()).hexdigest()
env.Append(CPPDEFINES=[("CROSSPOINT_TINYUSB_CONFIG_HASH", '"' + digest + '"')])
