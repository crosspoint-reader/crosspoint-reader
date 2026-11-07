"""Compile production event/HTTP code with deterministic SD, clock and transport stubs."""
import argparse
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--arduinojson", type=Path, default=root / ".pio/libdeps/x4pro/ArduinoJson/src",
                    help="ArduinoJson 7.4.2 src directory (from the PlatformIO dependency cache)")
parser.add_argument("--sanitize", action="store_true", help="enable address and undefined-behavior sanitizers")
args = parser.parse_args()
if not (args.arduinojson / "ArduinoJson.h").is_file():
    parser.error("ArduinoJson headers missing; supply --arduinojson with the cached src directory")
flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
with tempfile.TemporaryDirectory(prefix="plugin-event-budget-") as tmp:
    binary = Path(tmp) / "drain"
    subprocess.run([
        "c++", "-std=c++20", "-O0", *flags, "-I", str(here / "stubs"),
        "-I", str(args.arduinojson), "-I", str(root / "src"),
        str(root / "src/util/PluginEvents.cpp"), str(root / "src/util/PluginHttp.cpp"),
        str(here / "DrainTest.cpp"), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
print("Plugin event cooperative budget host checks passed")
