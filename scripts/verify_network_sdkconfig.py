"""
PlatformIO post-build guard for LwIP throughput Kconfig overrides.

The ESP-IDF networking stack bakes several LwIP buffer sizes into compiled
objects. A generated sdkconfig.<env> with package defaults can silently produce
much slower downloads, so fail the build if the generated sdkconfig.h does not
match the source-of-truth overrides in sdkconfig.defaults.
"""

from pathlib import Path

Import("env")  # noqa: F821  -- provided by PlatformIO at script load


REQUIRED_KEYS = (
    "CONFIG_LWIP_TCP_SND_BUF_DEFAULT",
    "CONFIG_LWIP_TCP_WND_DEFAULT",
    "CONFIG_LWIP_TCP_RECVMBOX_SIZE",
    "CONFIG_LWIP_UDP_RECVMBOX_SIZE",
    "CONFIG_LWIP_TCPIP_RECVMBOX_SIZE",
    "CONFIG_FREERTOS_HZ",
    "CONFIG_AUTOSTART_ARDUINO",
    "CONFIG_ESPTOOLPY_FLASHSIZE_16MB",
)


def parse_sdkconfig_defaults(path):
    values = {}
    with path.open("r", encoding="utf-8") as config:
        for raw_line in config:
            line = raw_line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            if key in REQUIRED_KEYS:
                values[key] = "1" if value == "y" else value
    return values


def parse_sdkconfig_h(path):
    values = {}
    with path.open("r", encoding="utf-8") as config:
        for raw_line in config:
            parts = raw_line.strip().split(maxsplit=2)
            if len(parts) >= 3 and parts[0] == "#define" and parts[1] in REQUIRED_KEYS:
                values[parts[1]] = parts[2]
    return values


def verify_network_sdkconfig(target, source, env):
    del target, source

    project_dir = Path(env.subst("$PROJECT_DIR")).resolve()
    sdkconfig_defaults = project_dir / "sdkconfig.defaults"
    build_dir = Path(env.subst("$BUILD_DIR")).resolve()
    sdkconfig_h = build_dir / "config" / "sdkconfig.h"

    expected = parse_sdkconfig_defaults(sdkconfig_defaults)
    generated = parse_sdkconfig_h(sdkconfig_h)

    missing_defaults = [key for key in REQUIRED_KEYS if key not in expected]
    mismatches = [
        (key, expected.get(key), generated.get(key))
        for key in REQUIRED_KEYS
        if expected.get(key) != generated.get(key)
    ]

    if missing_defaults or mismatches:
        print("ERROR: network sdkconfig overrides were not applied")
        if missing_defaults:
            print("Missing from sdkconfig.defaults:")
            for key in missing_defaults:
                print(f"  {key}")
        if mismatches:
            print("Mismatched generated values:")
            for key, expected_value, actual_value in mismatches:
                print(f"  {key}: expected {expected_value}, got {actual_value}")
        env.Exit(1)

    print(f"Verified network sdkconfig overrides in {sdkconfig_h}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}${PROGSUFFIX}", verify_network_sdkconfig)  # noqa: F821
