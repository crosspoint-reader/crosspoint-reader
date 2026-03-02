"""
Post-build script: patch esp_app_desc_t.version in firmware.bin.

The ESP-IDF app description structure is what web flashers (e.g. xteink.dve.al)
read to display the firmware version. Arduino-ESP32 sets this to the
arduino-lib-builder SHA, so we overwrite it with CROSSPOINT_VERSION after linking.
"""
Import("env")
import os
import struct


def patch_app_version(source, target, env):
    # Reconstruct the version string from build variables.
    # GIT_SHA is set by git_version.py as e.g. \"ff6d74c\" (with escaped quotes).
    sha = "unknown"
    for define in env.get("CPPDEFINES", []):
        if isinstance(define, (list, tuple)) and len(define) == 2:
            if define[0] == "GIT_SHA":
                sha = str(define[1]).strip('\\"').strip('"')

    base_version = "1.1.0"
    try:
        config = env.GetProjectConfig()
        base_version = config.get("crosspoint", "version")
    except Exception:
        pass

    env_name = env["PIOENV"]
    if env_name == "gh_release":
        version = base_version
    elif env_name == "gh_release_rc":
        rc_hash = os.environ.get("CROSSPOINT_RC_HASH", sha)
        version = f"{base_version}-rc+{rc_hash}"
    elif env_name == "slim":
        version = f"{base_version}-slim"
    else:
        version = f"{base_version}-dev+{sha}"

    firmware_path = str(target[0])
    with open(firmware_path, "r+b") as f:
        data = f.read()

    magic = struct.pack("<I", 0xABCD5432)
    pos = data.find(magic)
    if pos < 0:
        print("[patch_app_version] WARNING: esp_app_desc magic not found — skipping")
        return

    # esp_app_desc_t layout: magic(4) + secure_version(4) + reserv1(8) + version[32]
    version_offset = pos + 16
    encoded = version.encode("utf-8")[:31]
    padded = encoded + b"\x00" * (32 - len(encoded))

    with open(firmware_path, "r+b") as f:
        f.seek(version_offset)
        f.write(padded)

    print(f"[patch_app_version] esp_app_desc version → {version}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", patch_app_version)
