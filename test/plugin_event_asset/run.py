"""Run the real BMP helper/decoder and production event drain with SD/HTTP boundaries.

Requires existing PlatformIO ArduinoJson dependencies; never installs packages.
Set ARDUINOJSON_SRC to an existing ArduinoJson/src directory when host-only.
The generated excerpts remove unrelated subscription/emit and HTTP transport
code, not manifest parsing, delivery, queue handling, or vocabulary helpers.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
arduinojson = Path(os.environ.get("ARDUINOJSON_SRC", root / ".pio/libdeps/x4pro/ArduinoJson/src"))
if not (arduinojson / "ArduinoJson.h").is_file():
    raise SystemExit("Existing ArduinoJson headers required; set ARDUINOJSON_SRC or prepare PlatformIO dependencies")
events = (root / "src/util/PluginEvents.cpp").read_text()
start = events.index("namespace {\n")
end = events.index("}  // namespace\n", start) + len("}  // namespace\n")
drain_start = events.index("namespace {\n\n// The manifest subset")
assert events.count("bool loadDrainManifest(") == 1
assert events.count("bool deliverLine(") == 1
assert events.count("void drain(") == 1
http = (root / "src/util/PluginHttp.cpp").read_text()
http_start = http.index("namespace pluginhttp {\n")
http_end = http.index("// A manifest may not point")
with tempfile.TemporaryDirectory(prefix="plugin-event-asset-") as tmp:
    tmp = Path(tmp)
    (tmp / "WString.h").write_text("#pragma once\n#include <string>\nusing String = std::string;\n")
    (tmp / "ProductionDrain.inc").write_text(events[start:end] + "\nnamespace pluginevents {\n" + events[drain_start:])
    (tmp / "ProductionHttp.inc").write_text(http[http_start:http_end] + "\n}  // namespace pluginhttp\n")
    binary = tmp / "check"
    subprocess.run([
        "c++", "-std=c++20", "-O0", "-g", "-fsanitize=address,undefined",
        "-I", str(tmp), "-I", str(here / "stubs"), "-I", str(arduinojson),
        "-I", str(root / "src"), "-I", str(root / "lib/GfxRenderer"), "-I", str(root / "lib/Memory"),
        str(root / "src/util/PluginEventAsset.cpp"),
        str(root / "lib/GfxRenderer/Bitmap.cpp"),
        str(root / "lib/GfxRenderer/BitmapHelpers.cpp"),
        str(here / "AssetTest.cpp"), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
print("Plugin event BMP helper/Bitmap and actual loadDrainManifest/deliverLine/drain host checks passed (ASan/UBSan)")
