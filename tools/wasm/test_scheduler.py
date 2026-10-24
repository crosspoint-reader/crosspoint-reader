#!/usr/bin/env python3
"""Exercise the browser's actual fiber scheduler in a small WASM executable."""

import argparse
import shutil
import subprocess
from pathlib import Path

from build import browser_simulator

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", choices=("x4", "x3", "x4pro"), default="x4")
    args = parser.parse_args()
    simulator = ROOT / ".cache/wasm" / args.device / "simulator"
    if not simulator.exists() or not shutil.which("em++"):
        parser.error("Activate Emscripten and build this device's preview first")
    browser_simulator(ROOT / ".pio/libdeps" / f"preview-{args.device}" / "simulator/src", simulator)
    output = ROOT / ".cache/wasm/scheduler-test.cjs"
    subprocess.run([
        "em++", "-std=c++20", "-O1", "-fno-exceptions", "-DENABLE_SERIAL_LOG",
        "-I" + str(simulator), "-I" + str(ROOT / "lib/Logging"),
        str(HERE / "src/BrowserScheduler.cpp"), str(HERE / "tests/SchedulerTest.cpp"),
        "-sASYNCIFY=1", "-sSTACK_SIZE=1MB", "-sASSERTIONS=1", "-sENVIRONMENT=node", "-o", str(output),
    ], check=True)
    subprocess.run(["node", str(output)], check=True, timeout=15)


if __name__ == "__main__":
    main()
