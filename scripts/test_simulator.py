#!/usr/bin/env python3
"""Build and smoke-test the CrossVi X3/X4 desktop simulator."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import shutil
import struct
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "simulator-tests"
GOLDEN_PATH = ROOT / "simulator" / "tests" / "golden_home.json"


def platformio() -> str:
    executable = shutil.which("pio") or shutil.which("platformio")
    if executable:
        return executable
    bundled = Path.home() / ".platformio" / "penv" / "bin" / "pio"
    if bundled.exists():
        return str(bundled)
    raise RuntimeError("PlatformIO is missing")


def run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
    print("+", shlex.join(command))
    return subprocess.run(command, cwd=ROOT, text=True, check=True, **kwargs)


def compile_and_run_test(name: str, sources: list[str], flags: list[str]) -> None:
    binary = BUILD / name
    run(
        [
            shutil.which("g++") or "g++",
            "-std=gnu++20",
            "-Isimulator/src",
            *sources,
            "-o",
            str(binary),
            *flags,
        ]
    )
    run([str(binary)])


def test_host_adapters() -> None:
    flags = shlex.split(
        subprocess.check_output([sys.executable, "scripts/simulator_build_flags.py"], cwd=ROOT, text=True)
    )
    BUILD.mkdir(parents=True, exist_ok=True)
    compile_and_run_test(
        "controls-test",
        ["simulator/tests/SimulatorControlsTest.cpp", "simulator/src/SimulatorControls.cpp"],
        flags,
    )
    compile_and_run_test(
        "input-test",
        [
            "simulator/tests/SimulatorInputTest.cpp",
            "simulator/src/HalGPIO.cpp",
            "simulator/src/SimulatorControls.cpp",
            "simulator/src/SimulatorLifecycle.cpp",
        ],
        flags,
    )
    compile_and_run_test(
        "storage-test",
        ["simulator/tests/SimulatorStorageTest.cpp", "simulator/src/HalStorage.cpp"],
        flags,
    )


def bmp_dimensions(path: Path) -> tuple[int, int]:
    header = path.read_bytes()[:26]
    if len(header) < 26 or header[:2] != b"BM":
        raise AssertionError(f"Not a BMP screenshot: {path}")
    width, height = struct.unpack_from("<ii", header, 18)
    return width, abs(height)


def smoke_device(device: str, expected: dict[str, object], update: bool) -> tuple[str, str]:
    output = BUILD / device
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    shots = output / "screenshots"
    sd.mkdir(parents=True)
    shots.mkdir(parents=True)

    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
            "CROSSVI_SIM_SCREENSHOT_AFTER_MS": "800",
            "CROSSVI_SIM_EXIT_AFTER_MS": "1200",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=15)
    log = completed.stdout + completed.stderr
    if f"Hardware detect: {device.upper()}" not in log or "Entering activity: Home" not in log:
        raise AssertionError(f"{device.upper()} did not boot to Home:\n{log}")

    bmps = list(shots.glob("*.bmp"))
    raw_files = list(shots.glob("*.framebuffer.bin"))
    if len(bmps) != 1 or len(raw_files) != 1:
        raise AssertionError(f"{device.upper()} produced an unexpected screenshot set")
    dimensions = bmp_dimensions(bmps[0])
    expected_dimensions = (int(expected["logical_width"]), int(expected["logical_height"]))
    if dimensions != expected_dimensions:
        raise AssertionError(f"{device.upper()} screenshot is {dimensions}, expected {expected_dimensions}")
    raw = raw_files[0].read_bytes()
    if len(raw) != int(expected["framebuffer_bytes"]):
        raise AssertionError(f"{device.upper()} framebuffer has {len(raw)} bytes")
    digest = hashlib.sha256(raw).hexdigest()
    if not update and digest != expected["sha256"]:
        raise AssertionError(
            f"{device.upper()} Home framebuffer changed: {digest}\n"
            "If this UI change is intentional, run scripts/test_simulator.py --update-golden."
        )
    bmp_digest = hashlib.sha256(bmps[0].read_bytes()).hexdigest()
    if not update and bmp_digest != expected["bmp_sha256"]:
        raise AssertionError(
            f"{device.upper()} logical Home screenshot changed: {bmp_digest}\n"
            "If this UI change is intentional, run scripts/test_simulator.py --update-golden."
        )
    print(
        f"{device.upper()}: Home {dimensions[0]}x{dimensions[1]}, {len(raw)} bytes, "
        f"raw sha256={digest}, BMP sha256={bmp_digest}"
    )
    return digest, bmp_digest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skip-build", action="store_true", help="reuse existing simulator binaries")
    parser.add_argument("--update-golden", action="store_true", help="accept the current empty-Home frame")
    args = parser.parse_args()

    run([sys.executable, "scripts/setup_simulator_deps.py"])
    test_host_adapters()
    if not args.skip_build:
        pio = platformio()
        run([pio, "run", "-e", "simulator_x3"])
        run([pio, "run", "-e", "simulator_x4"])

    golden = json.loads(GOLDEN_PATH.read_text(encoding="utf-8"))
    for device in ("x3", "x4"):
        raw_digest, bmp_digest = smoke_device(device, golden[device], args.update_golden)
        golden[device]["sha256"] = raw_digest
        golden[device]["bmp_sha256"] = bmp_digest
    if args.update_golden:
        GOLDEN_PATH.write_text(json.dumps(golden, indent=2) + "\n", encoding="utf-8")
        print(f"Updated {GOLDEN_PATH.relative_to(ROOT)}")
    print("Simulator checks passed.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
