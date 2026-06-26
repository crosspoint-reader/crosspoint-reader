"""Give the backward-cpp dependency a PlatformIO manifest so it builds clean.

backward-cpp (pulled via lib_deps) ships no library.json and keeps backward.cpp
plus its test/ and test_package/ trees at the repo root. Without a manifest
PlatformIO's Library Dependency Finder compiles everything it finds, and
test_package/main.cpp (`#include <backward/backward.hpp>`) breaks the build.

We only consume the single header backward.hpp (for in-process symbol/line
resolution in the heap tracer), so drop a tiny manifest into the installed
package that marks it header-only via srcFilter. The package lives under the
gitignored .pio/libdeps and is recreated on reinstall, so this runs as an
idempotent pre-build action after dependencies are installed.
"""

import json
import os

Import("env")  # noqa: F821 - provided by PlatformIO/SCons

_MANIFEST = {
    "name": "backward",
    "version": "1.6.0",
    "build": {
        # Header-only for our use: compile none of backward.cpp/test/test_package.
        # The package root stays on the include path so `#include "backward.hpp"`
        # still resolves.
        "srcFilter": ["-<*>", "+<backward.hpp>"],
    },
}


def _write_manifest() -> None:
    libdeps = env.subst("$PROJECT_LIBDEPS_DIR")  # noqa: F821
    pioenv = env.subst("$PIOENV")  # noqa: F821
    root = os.path.join(libdeps, pioenv, "backward")
    if not os.path.isdir(root):
        return  # dependency not installed (yet); nothing to do

    manifest_path = os.path.join(root, "library.json")
    try:
        with open(manifest_path, "w", encoding="utf-8") as fh:
            json.dump(_MANIFEST, fh, indent=2)
    except OSError:
        pass


_write_manifest()
