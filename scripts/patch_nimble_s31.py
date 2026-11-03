"""
PlatformIO pre: script: build NimBLE-Arduino on the ESP32-S31 against ESP-IDF's
own NimBLE host.

NimBLE-Arduino 2.5.1 bundles a NimBLE host and porting layer for the ESP-IDF 5
targets. On the S31 (ESP-IDF 6.1) the controller and host talk through the new
btdm OSAL, and the bundled port does not compile there. The prebuilt libbt.a
already holds ESP-IDF's host, so on the S31 only the library's C++ wrapper is
built, against that host's headers, as esp-nimble-cpp is under ESP-IDF:
  * nimconfig.h stops defining USING_NIMBLE_ARDUINO_HEADERS on the S31, so the
    wrapper includes "host/ble_gap.h" and friends from ESP-IDF
  * src/syscfg/syscfg.h hands over to ESP-IDF's with #include_next: the
    library's src/ comes first on the include path, and the bundled one would
    shadow it
  * the bundled host's sources (src/nimble/) are left out of the library build

All of it happens in a build middleware. Library builders clone this
environment, and PlatformIO runs the middleware while it collects the library's
sources: after the library is installed and scanned, before anything compiles.
A post: script would come too late to leave sources out. Nothing is moved, since
the dependency scan has already recorded the bundled headers.

The checks follow patch_sdfat.py: the library must be this environment's copy
under .pio/libdeps/<env>/; library.properties must say version 2.5.1; each
patched file must hash to the reviewed upstream or patched bytes. Anything else
fails the build. Already-patched files are left untouched.
"""

import hashlib
import os
from pathlib import Path


LIBRARY = "NimBLE-Arduino"
VERSION = "version=2.5.1"
ARDUINO_HEADERS = b"#define USING_NIMBLE_ARDUINO_HEADERS (1)\n"
# (file, upstream sha256, patched sha256, upstream bytes, patched bytes)
PATCHES = (
    ("src/nimconfig.h",
     "c4caa5fe877b734349dce027ba5037e5e875df085931eb72404ecf0fa10cc830",
     "a6b93d63dc32258caa1424c3eaf80acc23dc779ef389a397c28ef57d9f04282c",
     ARDUINO_HEADERS,
     b"#ifndef CONFIG_IDF_TARGET_ESP32S31\n" + ARDUINO_HEADERS + b"#endif\n"),
    ("src/syscfg/syscfg.h",
     "54af358bbcefdbf12903b626693111acb898fadf7aa576895c426aadbfc42a54",
     "9a37e78ef761593f911fafbbbc26fd475044dcf62ceb3a663e137b5d0a3eaa7a",
     b'#include "nimble/porting/nimble/include/syscfg/syscfg.h"\n',
     b"// ESP32-S31: the host is ESP-IDF's, with its own syscfg (scripts/patch_nimble_s31.py).\n"
     b"#include_next <syscfg/syscfg.h>\n"),
)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def patch_library(dependency):
    properties = dependency / "library.properties"
    targets = [properties] + [dependency / patch[0] for patch in PATCHES]
    for target in targets:
        if not target.is_file():
            raise RuntimeError("Missing NimBLE-Arduino patch target: " + target.name)
    if VERSION not in properties.read_text().splitlines():
        raise RuntimeError("The S31 NimBLE-Arduino patch requires " + VERSION)

    # Check every file before changing any.
    pending = []
    for relative, upstream, patched, old, new in PATCHES:
        target = dependency / relative
        source = target.read_bytes()
        current = digest(source)
        if current not in (upstream, patched):
            raise RuntimeError("Unrecognized NimBLE-Arduino source: " + relative)
        if current == upstream:
            result = source.replace(old, new, 1)
            if digest(result) != patched:
                raise RuntimeError("Unexpected patched NimBLE-Arduino source: " + relative)
            pending.append((target, result))
    for target, result in pending:
        target.write_bytes(result)
        print("Patched NimBLE-Arduino " + target.name + " for the ESP32-S31")


def add_middleware(env):
    # PioArduino's SDK-only pass builds no libraries.
    if env.get("ARDUINO_LIB_COMPILE_FLAG") == "Build":
        return
    if env.BoardConfig().get("build.mcu", "") != "esp32s31":
        raise RuntimeError("patch_nimble_s31.py is for ESP32-S31 environments only")
    dependency = (Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env["PIOENV"] / LIBRARY).resolve()
    library_src = os.path.normcase(str(dependency / "src")) + os.sep
    bundled_host = os.path.normcase(str(dependency / "src" / "nimble")) + os.sep
    patched = []

    def s31_nimble_sources(node):
        source = os.path.normcase(node.srcnode().get_abspath())
        if not source.startswith(library_src):
            return node
        if not patched:
            patch_library(dependency)
            patched.append(True)
        return None if source.startswith(bundled_host) else node

    env.AddBuildMiddleware(s31_nimble_sources)


if "Import" in globals():
    Import("env")  # noqa: F821 -- supplied by SCons
    add_middleware(env)  # noqa: F821
