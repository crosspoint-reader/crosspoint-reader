#!/usr/bin/env python3
"""Build and launch the pixel-accurate CrossVi X3/X4 desktop simulator."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def platformio() -> str | None:
    executable = shutil.which("pio") or shutil.which("platformio")
    if executable:
        return executable
    bundled = Path.home() / ".platformio" / "penv" / "bin" / "pio"
    return str(bundled) if bundled.exists() else None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("device", choices=("x3", "x4"), help="Xteink device profile")
    parser.add_argument("--build-only", action="store_true", help="compile without opening a window")
    parser.add_argument("--sd", type=Path, help="directory used as the virtual SD card")
    parser.add_argument("--screenshot-dir", type=Path, help="directory for F12 framebuffer captures")
    args = parser.parse_args()

    pio = platformio()
    if not pio:
        print("PlatformIO is missing. Install PlatformIO Core, then retry.", file=sys.stderr)
        return 1

    deps = subprocess.run([sys.executable, str(ROOT / "scripts" / "setup_simulator_deps.py")], cwd=ROOT)
    if deps.returncode:
        return deps.returncode

    environment = f"simulator_{args.device}"
    build = subprocess.run([pio, "run", "-e", environment], cwd=ROOT)
    if build.returncode or args.build_only:
        return build.returncode

    binary = ROOT / ".pio" / "build" / environment / "program"
    sim_env = os.environ.copy()
    sd_root = (args.sd or ROOT / ".simulator-data" / args.device).resolve()
    sd_root.mkdir(parents=True, exist_ok=True)
    sim_env["CROSSVI_SIM_SD"] = str(sd_root)
    if args.screenshot_dir:
        screenshot_dir = args.screenshot_dir.resolve()
        screenshot_dir.mkdir(parents=True, exist_ok=True)
        sim_env["CROSSVI_SIM_SCREENSHOT_DIR"] = str(screenshot_dir)
    return subprocess.run([str(binary)], cwd=ROOT, env=sim_env).returncode


if __name__ == "__main__":
    raise SystemExit(main())
