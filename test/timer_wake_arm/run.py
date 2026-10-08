#!/usr/bin/env python3
"""Exercise the production sleep function with controlled IDF return values."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--negative-control", action="store_true", help="also check the original PR head rejects the regression test")
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
baseline = "3678609ff49a6a082c5181f1366ebf14038a836c"


def function(source):
    start = source.index("void HalPowerManager::startDeepSleep(")
    end = source.index("uint16_t HalPowerManager::getBatteryPercentage()", start)
    return source[start:end]


with tempfile.TemporaryDirectory(prefix="timer-wake-arm-") as temporary:
    temp = Path(temporary)
    source = (root / "lib/hal/HalPowerManager.cpp").read_text()
    variants = [("candidate", source)]
    if args.negative_control:
        original = subprocess.check_output(
            ["git", "show", baseline + ":lib/hal/HalPowerManager.cpp"], cwd=root, text=True
        )
        variants.append(("original", original))
    for name, source in variants:
        (temp / "SleepSource.inc").write_text(function(source))
        for ext1 in (0, 1):
            executable = temp / (name + str(ext1))
            subprocess.run(
                [os.environ.get("CXX", "c++"), "-std=c++20", "-O0", "-Wall", "-Wextra",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-DSOC_PM_SUPPORT_EXT1_WAKEUP=" + str(ext1), "-I", str(temp),
                 str(here / "TimerWakeArmTest.cpp"), "-o", str(executable)], check=True
            )
            command = [str(executable)] + (["negative-control"] if name == "original" else [])
            result = subprocess.run(command)
            expected = 42 if name == "original" else 0
            if result.returncode != expected:
                raise RuntimeError(f"{name}/ext1={ext1}: exit {result.returncode}, expected {expected}")
            print(f"{name}/ext1={ext1}: expected outcome confirmed")
