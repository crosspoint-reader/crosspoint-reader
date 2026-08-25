"""
PlatformIO pre-build script: make the arduino-esp32 core define _btLibraryInUse
in a BLE-controller-only build.

The core header cores/esp32/esp32-hal-bt-mem.h emits a constructor
(_setBtLibraryInUse) that references `_btLibraryInUse` whenever BLE hardware is
present, but the core only DEFINES that symbol (in esp32-hal-bt.c) when an IDF
host stack -- CONFIG_BLUEDROID_ENABLED or CONFIG_NIMBLE_ENABLED -- is enabled.

CrossPoint runs the ESP-IDF BLE *controller* only, with NimBLE-Arduino supplying
the host (both IDF host stacks are disabled in custom_sdkconfig to avoid duplicate
npl_freertos_* symbols). In that configuration the core never defines the symbol,
so NimBLE's constructor link-fails with "undefined reference to `_btLibraryInUse'"
-- and it fails in links that application source cannot reach (the final firmware
link and the arduino-lib-builder core-rebuild's dummy firmware).

Fix: turn the header's `extern bool _btLibraryInUse;` DECLARATION into a weak
DEFINITION. Every translation unit that includes the header (NimBLE) then provides
the symbol itself; the linker merges the duplicate weak defs, and the core's own
strong definition still wins whenever a host stack is actually enabled. It must
live in the header, not our src, because only the header is seen by NimBLE's TU in
every link.

Idempotent text edit. Patching bumps the header mtime, so NimBLE recompiles
against it on the next build.
"""

Import("env")  # noqa: F821 (SCons-injected global)
import glob
import os

EXTERN_DECL = "extern bool _btLibraryInUse;"
MARKER = "patch_bt_mem.py"
WEAK_DEF = (
    "__attribute__((weak)) bool _btLibraryInUse = false;  /* CrossPoint " + MARKER + " */"
)


def _find_header(env):
    rel = os.path.join("cores", "esp32", "esp32-hal-bt-mem.h")
    candidates = []

    # 1) Ask the platform for the framework package dir (works when the name matches).
    try:
        fw = env.PioPlatform().get_package_dir("framework-arduinoespressif32")  # noqa: F821
        if fw:
            candidates.append(os.path.join(fw, rel))
    except Exception:
        pass

    # 2) Glob the packages dir(s) -- robust to the exact package name/version.
    pkg_dirs = []
    for key in ("PROJECT_PACKAGES_DIR", "PROJECT_CORE_DIR"):
        try:
            d = env.subst("$%s" % key)  # noqa: F821
            if d:
                pkg_dirs.append(d)
        except Exception:
            pass
    pkg_dirs.append(os.path.expanduser(os.path.join("~", ".platformio", "packages")))
    for base in pkg_dirs:
        pattern = os.path.join(base, "**", "framework-arduinoespressif32*", rel)
        candidates.extend(glob.glob(pattern, recursive=True))

    for c in candidates:
        if os.path.isfile(c):
            return c
    return None


def patch_bt_mem(env):
    header = _find_header(env)
    if not header:
        print("patch_bt_mem: esp32-hal-bt-mem.h not found; skipping (controller-only "
              "link may fail on _btLibraryInUse)")
        return

    with open(header, "r") as f:
        text = f.read()

    if MARKER in text:
        print("patch_bt_mem: already patched (%s)" % header)
        return
    if EXTERN_DECL not in text:
        print("patch_bt_mem: '%s' not found in %s; NOT patching (upstream header "
              "changed -- update this script)" % (EXTERN_DECL, header))
        return

    text = text.replace(EXTERN_DECL, WEAK_DEF, 1)
    with open(header, "w") as f:
        f.write(text)
    print("patch_bt_mem: patched %s (_btLibraryInUse now weak-defined)" % header)


patch_bt_mem(env)  # noqa: F821
