"""Work around pioarduino's custom-SDK libc linker configuration gap.

In pioarduino 55.03.311, custom_sdkconfig rebuilds SDK libraries and configuration
headers, but the Arduino application still uses the packaged libc LINKFLAGS from
esp32c3/pioarduino-build.py. Selecting ROM libc therefore also requires updating
those flags. Remove this hook once pioarduino derives the application's libc
linker flags from the rebuilt SDK configuration.
"""
from pathlib import Path


def configure_rom_libc(env):
    if env.BoardConfig().get("build.mcu") != "esp32c3":
        return
    # The outer core-generation pass has not installed its new sdkconfig yet.
    if env.get("ARDUINO_LIB_COMPILE_FLAG") == "Build":
        return
    requested = env.GetProjectOption("custom_sdkconfig", "")
    libc_setting = None
    for line in requested.splitlines():
        key, separator, value = line.strip().partition("=")
        if separator and key == "CONFIG_LIBC_OPTIMIZED_MISALIGNED_ACCESS":
            libc_setting = value.strip()
    if libc_setting != "n":
        return

    libs = Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs")) / "esp32c3"
    config = (libs / "sdkconfig").read_text(encoding="utf-8").splitlines()
    if "# CONFIG_LIBC_OPTIMIZED_MISALIGNED_ACCESS is not set" not in config:
        raise RuntimeError("C3 framework sdkconfig does not match the requested ROM libc configuration")
    rom = libs / "ld" / "esp32c3.rom.libc-suboptimal_for_misaligned_mem.ld"
    if not rom.is_file():
        raise RuntimeError(f"Missing C3 ROM libc linker script: {rom}")

    # pioarduino keeps the prebuilt package's -u anchors after a custom SDK rebuild.
    # Mirror ESP-IDF's esp_rom selection so flash-off callers still reach ROM.
    functions = ("memcpy", "memmove", "memcmp", "strcpy", "strncpy", "strncmp", "strcmp")
    anchors = {f"esp_libc_include_{name}_impl" for name in functions}
    flags = list(env["LINKFLAGS"])
    filtered = []
    index = 0
    while index < len(flags):
        if flags[index] == "-u" and index + 1 < len(flags) and flags[index + 1] in anchors:
            index += 2
        else:
            filtered.append(flags[index])
            index += 1
    if str(rom) not in filtered:
        filtered.extend(["-T", str(rom)])
    env.Replace(LINKFLAGS=filtered)


Import("env")  # noqa: F821 -- provided by PlatformIO
configure_rom_libc(env)  # noqa: F821
