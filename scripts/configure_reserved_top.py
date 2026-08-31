"""Validate and inject the optional damaged-screen reservation percentage."""

import os

Import("env")  # noqa: F821  -- provided by PlatformIO at script load


VARIABLE = "CROSSPOINT_RESERVED_TOP_PERCENT"
raw_percent = os.environ.get(VARIABLE, "").strip()

try:
    percent = int(raw_percent)
except ValueError as exc:
    raise ValueError(f"{VARIABLE} must be an integer from 1 through 50") from exc

if not 1 <= percent <= 50:
    raise ValueError(f"{VARIABLE} must be an integer from 1 through 50")

env.Append(CPPDEFINES=[(VARIABLE, percent)])  # noqa: F821
print(f"CrossPoint reserved top band: {percent}%")
