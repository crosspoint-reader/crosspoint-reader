import hashlib
import importlib.util
import sys
from pathlib import Path


def cache_matches(requested, cached_request):
    return bool(requested.strip()) and requested.strip() == cached_request.strip()


if "--self-test" in sys.argv:
    assert cache_matches("CONFIG_A=y\nCONFIG_B=n", "CONFIG_A=y\nCONFIG_B=n")
    assert not cache_matches("CONFIG_A=y", "CONFIG_A=y\nCONFIG_B=n")
    raise SystemExit(0)


Import("env")  # noqa: F821 -- provided by PlatformIO

platform = env.PioPlatform()
builder = Path(platform.get_dir()) / "builder" / "frameworks" / "arduino.py"
source = builder.read_text(encoding="utf-8")
# The package already stores libraries per chip; only its cache-presence check is global.
old_check = '''flag_any_custom_sdkconfig = (FRAMEWORK_LIB_DIR is not None and 
                            exists(str(Path(FRAMEWORK_LIB_DIR) / "sdkconfig")))'''
new_check = '''flag_any_custom_sdkconfig = (
    FRAMEWORK_LIB_DIR is not None
    and exists(str(Path(FRAMEWORK_LIB_DIR) / chip_variant / "sdkconfig.orig"))
)'''

# pioarduino 55.03.39 removed trailing whitespace from the same expression.
if old_check not in source:
    old_check = old_check.replace("and \n", "and\n")

if old_check in source:
    builder.write_text(source.replace(old_check, new_check, 1), encoding="utf-8")
elif new_check not in source:
    raise RuntimeError("Unsupported pioarduino cache check")

requested = env.GetProjectOption("custom_sdkconfig", "")
board = env.BoardConfig()
mcu = board.get("build.mcu", "esp32")
chip_variant = board.get("build.chip_variant", "").lower() or mcu
component_spec = importlib.util.spec_from_file_location(
    "crosspoint_pio_components", builder.with_name("component_manager.py")
)
components = importlib.util.module_from_spec(component_spec)
component_spec.loader.exec_module(components)
fingerprint_fn = getattr(components, "board_memory_fingerprint", None)
memory_fingerprint = fingerprint_fn(env, board) if fingerprint_fn else ""
cache_request = requested.strip() + memory_fingerprint if requested.strip() else ""
framework_libs = Path(platform.get_package_dir("framework-arduinoespressif32-libs"))
target_sdkconfig = framework_libs / chip_variant / "sdkconfig"
original_sdkconfig = framework_libs / chip_variant / "sdkconfig.orig"
request_file = framework_libs / chip_variant / "sdkconfig.crosspoint"

rebuild = bool(requested.strip())
if original_sdkconfig.is_file():
    cached_request = (
        request_file.read_text(encoding="utf-8") if request_file.is_file() else ""
    )
    if cache_matches(cache_request, cached_request):
        rebuild = False
        marker = "# TASMOTA__" + hashlib.md5(
            (requested.strip() + mcu + memory_fingerprint).encode("utf-8")
        ).hexdigest()[:16]
        defaults = Path(env.subst("$PROJECT_DIR")) / "sdkconfig.defaults"
        lines = (
            defaults.read_text(encoding="utf-8").splitlines()
            if defaults.exists()
            else []
        )
        if not lines or lines[0] != marker:
            defaults.write_text(
                "\n".join([marker, *lines[1:]]) + "\n", encoding="utf-8"
            )
            print(f"Restored cached Arduino framework for {mcu}")
    elif requested.strip():
        # Recompile only this chip from its original template after config changes.
        original_sdkconfig.replace(target_sdkconfig)
    # With no custom request, retain .orig so upstream reinstalls prebuilt archives.

if rebuild and not board.get("build.esp-idf.sdkconfig_path", ""):
    # Resolved Kconfig values override sdkconfig.defaults, even across SDK upgrades.
    # Only reset the generated file for this environment; explicit paths are user-owned.
    resolved = Path(env.subst("$PROJECT_DIR")) / f"sdkconfig.{env.subst('$PIOENV')}"
    if resolved.is_file() and "# Automatically generated file. DO NOT EDIT." in (
        resolved.read_text(encoding="utf-8").splitlines()[:5]
    ):
        resolved.unlink()
        print(f"Reset generated SDK configuration: {resolved.name}")

request_file.write_text(cache_request + "\n", encoding="utf-8")
