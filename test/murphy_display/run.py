#!/usr/bin/env python3
"""Compile the real Murphy panel driver against a recording bus and run it."""
import os
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
test = repo / "test/murphy_display"
display = repo / "freeink-sdk/libs/display/FreeInkDisplay/src"
with tempfile.TemporaryDirectory(prefix="murphy-display-") as temp:
    executable = str(Path(temp) / "test")
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter", "-I", str(test / "stubs"), "-I", str(display),
        str(test / "MurphyDisplayTest.cpp"),
        str(display / "driver/Uc8253MurphyDriver.cpp"), "-o", executable,
    ], check=True)
    subprocess.run([executable], check=True)
