#!/usr/bin/env python3
"""
Workflow helper for build-custom.yml.

Keeps GitHub Actions input parsing compact while delegating feature truth to
generate_build_config.py.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

from generate_build_config import FEATURES, PROFILES, resolve_profile_name


def parse_feature_list(raw: str) -> list[str]:
    return [token for token in re.split(r"[\s,]+", raw.strip()) if token]


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate platformio-custom.ini for the custom workflow")
    parser.add_argument("--profile", default="standard")
    parser.add_argument("--enable-features", default="")
    parser.add_argument("--disable-features", default="")
    parser.add_argument("--output", default="platformio-custom.ini")
    args = parser.parse_args()

    requested_profile = resolve_profile_name(args.profile)
    if requested_profile != "custom" and requested_profile not in PROFILES:
        print(f"Unknown profile: {args.profile}", file=sys.stderr)
        return 1

    enable_features = parse_feature_list(args.enable_features)
    disable_features = parse_feature_list(args.disable_features)

    unknown = sorted({feature for feature in enable_features + disable_features if feature not in FEATURES})
    if unknown:
        print(
            "Unknown feature key(s): " + ", ".join(unknown) + ". "
            "Use feature keys from scripts/generate_build_config.py.",
            file=sys.stderr,
        )
        return 1

    command: list[str] = [sys.executable, "scripts/generate_build_config.py"]
    if requested_profile != "custom":
        command.extend(["--profile", requested_profile])
    for feature in enable_features:
        command.extend(["--enable", feature])
    for feature in disable_features:
        command.extend(["--disable", feature])
    command.extend(["--output", args.output])

    print("Executing:", " ".join(command))
    completed = subprocess.run(command)
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
