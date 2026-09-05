"""Host libraries for the standalone native simulator configuration."""

import sys

Import("env")  # noqa: F821 -- supplied by PlatformIO/SCons

if sys.platform.startswith("linux"):
    env.Append(LIBS=["ssl", "crypto"])  # noqa: F821
elif sys.platform != "darwin":
    raise RuntimeError("The simulator requires macOS or Linux (including WSL)")
