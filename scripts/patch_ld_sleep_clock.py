"""
PlatformIO pre-build script: fix an upstream linker-script omission that makes
CONFIG_BT_LE_SLEEP_ENABLE unbuildable on the ESP32-C61 Arduino framework.

Background
----------
Enabling the BLE controller's modem-sleep (CONFIG_BT_LE_SLEEP_ENABLE=y) links in
`sleep_clock_modem_retention_init` from libesp_hw_support.a. The C61 linker
script `ld/sections.ld` places `sleep_clock.*` .text via an *explicit* rule that
lists only two sections:

    *libesp_hw_support.a:sleep_clock.*(.text
        .text.__esp_system_init_fn_sleep_clock_startup_init
        .text.sleep_clock_system_retention_init)

and the generic `.text.*` collector in `.flash.text` EXCLUDE_FILEs
`sleep_clock.*`. So `.text.sleep_clock_modem_retention_init` matches neither the
explicit list nor the generic collector. It falls through to an output section
with no 4-byte alignment, landing at an odd flash address (e.g. 0x421e9b62).
esptool then rejects the image: "Invalid .text.sleep_clock_modem_retention_init
segment length ... has to be multiple of 4".

With BLE sleep disabled (the Arduino default) that section is never linked, so
the omission is invisible — it only bites once we enable BT_LE_SLEEP_ENABLE.

Fix
---
Add `.text.sleep_clock_modem_retention_init` to the explicit sleep_clock rule so
it is collected alongside the other retention-init sections in the properly
aligned .flash.text output section.

This patches a file inside the PlatformIO package cache (outside the repo). It
is idempotent and refuses to act if the upstream rule text has changed, so a
framework update that restructures the script won't be silently corrupted.
"""

Import("env")  # noqa: F821 (SCons-injected global)
import os

RULE_OLD = (
    "*libesp_hw_support.a:sleep_clock.*(.text "
    ".text.__esp_system_init_fn_sleep_clock_startup_init "
    ".text.sleep_clock_system_retention_init)"
)
RULE_NEW = (
    "*libesp_hw_support.a:sleep_clock.*(.text "
    ".text.__esp_system_init_fn_sleep_clock_startup_init "
    ".text.sleep_clock_system_retention_init "
    ".text.sleep_clock_modem_retention_init)"
)


def _framework_libs_dir(env):
    # e.g. ~/.platformio/packages/framework-arduinoespressif32-libs
    for pkg in ("framework-arduinoespressif32-libs",):
        d = env.PioPlatform().get_package_dir(pkg)
        if d and os.path.isdir(d):
            return d
    return None


def patch_sections_ld(env):
    libs_dir = _framework_libs_dir(env)
    if not libs_dir:
        return  # not the C61-from-source build; nothing to patch

    ld_path = os.path.join(libs_dir, "esp32c61", "ld", "sections.ld")
    if not os.path.isfile(ld_path):
        return

    with open(ld_path, "r", encoding="utf-8") as f:
        text = f.read()

    if ".text.sleep_clock_modem_retention_init" in text:
        return  # already patched

    if RULE_OLD not in text:
        # Upstream layout changed — do not risk corrupting it. If BT_LE_SLEEP is
        # enabled the build will still fail loudly at esptool, which is the
        # correct signal to revisit this patch.
        print(
            "WARN: patch_ld_sleep_clock: expected sleep_clock rule not found in "
            "%s; skipping (framework may have fixed or restructured this)." % ld_path
        )
        return

    text = text.replace(RULE_OLD, RULE_NEW, 1)
    with open(ld_path, "w", encoding="utf-8") as f:
        f.write(text)
    print("Patched sections.ld: collect .text.sleep_clock_modem_retention_init (%s)" % ld_path)


patch_sections_ld(env)  # noqa: F821
