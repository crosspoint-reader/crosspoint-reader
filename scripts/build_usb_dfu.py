Import("env")
from pathlib import Path
if env.BoardConfig().get("build.mcu") == "esp32c3":
    base = Path(env.subst("$PROJECT_DIR")) / "vendor/x3-usb-dfu"
    env.Prepend(CPPPATH=[str(base / "config")])
    env.Append(CPPPATH=[str(base / "src"), str(base / "tinyusb/src")])
    env.Append(CPPPATH=[str(base.parent.parent / "lib/Memory"), str(base.parent.parent / "lib/Logging")])
    env.BuildSources("$BUILD_DIR/x3-usb-dfu", str(base / "src"))

elif "-DCROSSPOINT_NATIVE_DFU=1" in env.Flatten(env.get("BUILD_FLAGS", [])):
    base = Path(env.subst("$PROJECT_DIR")) / "vendor"
    env.Append(CPPPATH=[str(base / "x3-usb-dfu/src"), str(base / "native-usb-dfu")])
    env.BuildSources("$BUILD_DIR/dfu-session", str(base / "x3-usb-dfu/src"), src_filter="+<DfuSession.cpp>")
    env.BuildSources("$BUILD_DIR/native-usb-dfu", str(base / "native-usb-dfu"))
