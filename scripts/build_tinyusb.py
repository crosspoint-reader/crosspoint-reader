"""Build the pinned TinyUSB device stack without changing the shared SDK package."""

import importlib.util
from pathlib import Path

Import("env")  # noqa: F821 -- PlatformIO

TINYUSB_REVISION = "53f8c53c2cbd73a91a172f1ae35e9abc00eb5075"


def configure_tinyusb(env):
    if env.BoardConfig().get("build.mcu") != "esp32s3":
        raise RuntimeError("The minimal TinyUSB build is only supported on ESP32-S3")
    if env.GetProjectOption("custom_sdkconfig", "").strip():
        raise RuntimeError("The minimal TinyUSB build requires the prebuilt S3 SDK")

    sdk = Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs"))
    versions = (sdk / "versions.txt").read_text(encoding="utf-8").splitlines()
    if "tinyusb: master " + TINYUSB_REVISION[:9] not in versions or "lib-builder: master ee57070" not in versions:
        raise RuntimeError("TinyUSB SDK revision changed; update and validate the minimal USB build")

    source = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV") / "TinyUSB" / "src"
    if not (source / "tusb.c").is_file():
        raise RuntimeError(f"Pinned TinyUSB dependency missing: {source}")

    tinyusb_env = env.Clone()
    tinyusb_env.Prepend(CPPPATH=[str(source)])
    tinyusb_env.Append(CCFLAGS=["-Wno-type-limits"])
    sources = [
        "tusb.c", "common/tusb_fifo.c", "device/usbd.c",
        "class/cdc/cdc_device.c", "class/msc/msc_device.c",
        "portable/synopsys/dwc2/dcd_dwc2.c", "portable/synopsys/dwc2/dwc2_common.c",
    ]
    library = tinyusb_env.BuildLibrary(
        env.subst("$BUILD_DIR") + "/tinyusb-device", str(source),
        src_filter=["-<*>"] + [f"+<{name}>" for name in sources],
    )
    libraries = list(env["LIBS"])
    if "-larduino_tinyusb" not in libraries:
        raise RuntimeError("SDK TinyUSB link entry changed; refusing to link mixed USB stacks")
    env.Replace(LIBS=[library if lib == "-larduino_tinyusb" else lib for lib in libraries])

    checker_path = Path(env.subst("$PROJECT_DIR")) / "scripts" / "check_tinyusb.py"
    spec = importlib.util.spec_from_file_location("crosspoint_check_tinyusb", checker_path)
    checker = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(checker)
    compiler = Path(env.WhereIs(env.subst("$CC")))
    nm = compiler.with_name(compiler.name.replace("gcc", "nm"))
    if not nm.is_file():
        raise RuntimeError(f"Missing toolchain symbol reader: {nm}")

    def check_link(source, target, env):
        checker.check_firmware(nm, target[0].get_abspath())
        print("TinyUSB link check passed (MSC/CDC only).")

    env.Depends("$BUILD_DIR/${PROGNAME}.elf", str(checker_path))
    env.AddPostAction(
        "$BUILD_DIR/${PROGNAME}.elf",
        env.VerboseAction(check_link, "Checking TinyUSB device-only linkage"),
    )
    print("Building pinned TinyUSB with device MSC/CDC only")


configure_tinyusb(env)  # noqa: F821
