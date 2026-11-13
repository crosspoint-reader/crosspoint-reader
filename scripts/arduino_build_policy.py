"""Select the Arduino libraries and managed components used by CrossPoint."""

import hashlib
import json
from pathlib import Path
import re
import shutil

import yaml


# DNSServer uses AsyncUDP; WebSockets uses NetworkClientSecure; WebServer uses FS and Hash.
ARDUINO_LIBRARIES = {
    "AsyncUDP", "DNSServer", "ESPmDNS", "FS", "Hash", "Network", "NetworkClientSecure",
    "Preferences", "SPI", "WebServer", "WiFi", "Wire",
}
# PNGdec's assembly includes a DSP platform header; discovery uses mDNS.
MANAGED_COMPONENTS = {"espressif/esp-dsp": "1.8.0", "espressif/mdns": "1.14.0"}
# __pio_env supplies PlatformIO's compiler metadata; core dumps have startup hooks.
IDF_COMPONENTS = (".dummy", "__pio_env", "framework-arduinoespressif32", "esp_wifi", "espcoredump")


def library_sdkconfig(kconfig, libraries):
    options = set(re.findall(r"^config ARDUINO_SELECTIVE_(\w+)$", kconfig, re.MULTILINE))
    options.discard("COMPILATION")
    if not options or libraries - options:
        raise RuntimeError(f"Unsupported Arduino library options: {sorted(libraries - options)}")
    return "\n".join([
        "CONFIG_ARDUINO_SELECTIVE_COMPILATION=y",
        *(f"CONFIG_ARDUINO_SELECTIVE_{name}={'y' if name in libraries else 'n'}"
          for name in sorted(options)),
    ])


def configure(env):
    requested = env.GetProjectOption("custom_sdkconfig", "")
    if not requested.strip():
        return

    framework = Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32"))
    manifest = framework / "idf_component.yml"
    original = manifest.with_suffix(".yml.orig")
    source = original.read_text() if original.exists() else manifest.read_text()
    data = yaml.safe_load(source)
    dependencies = data["dependencies"]
    missing = MANAGED_COMPONENTS.keys() - dependencies.keys()
    if missing:
        raise RuntimeError(f"Required Arduino components missing from manifest: {sorted(missing)}")

    # PioArduino's generated sketch also has a sample-only dependency manifest.
    scaffold = Path(env.subst("$PROJECT_DIR")) / ".dummy"
    if not scaffold.exists():
        shutil.copytree(Path(env.PioPlatform().get_dir()) / "builder/build_lib", scaffold)
    (scaffold / "idf_component.yml").unlink(missing_ok=True)

    board = env.BoardConfig()
    roots = list(IDF_COMPONENTS)
    if board.get("build.mcu") == "esp32s3":
        roots.append("esp_psram")
    cmake_args = board.get("build.cmake_extra_args", "")
    if "-DCOMPONENTS" in cmake_args or "-DMINIMAL_BUILD" in cmake_args:
        raise RuntimeError("CrossPoint owns the Arduino rebuild's component roots")
    board.update("build.cmake_extra_args", cmake_args + " -DCOMPONENTS=" + ";".join(roots))

    selective = library_sdkconfig((framework / "Kconfig.projbuild").read_text(), ARDUINO_LIBRARIES)
    # Include the entire upstream manifest so dependency changes invalidate cached SDKs.
    fingerprint = hashlib.sha256(json.dumps(
        [source, selective, MANAGED_COMPONENTS, roots], sort_keys=True,
    ).encode()).hexdigest()
    requested += "\n" + selective + "\n# CrossPoint build policy: " + fingerprint
    section = "env:" + env.subst("$PIOENV")
    config = env.GetProjectConfig()
    config.set(section, "custom_sdkconfig", requested)
    config.set(section, "custom_component_remove", "\n".join(
        sorted(dependencies.keys() - {"idf"} - MANAGED_COMPONENTS.keys())
    ))

    # PioArduino restores this backup after the rebuild, including its nested run.
    if not original.exists():
        original.write_text(source)
    for name, version in MANAGED_COMPONENTS.items():
        dependencies[name]["version"] = version
    manifest.write_text(yaml.safe_dump(data))
