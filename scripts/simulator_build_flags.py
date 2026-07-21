#!/usr/bin/env python3
"""Print native compiler/linker flags required by the CrossVi simulator."""

from __future__ import annotations

import glob
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LOCAL_ROOT = ROOT / ".cache" / "simulator-deps" / "root"


def command_flags(command: list[str]) -> list[str]:
    return shlex.split(subprocess.check_output(command, text=True).strip())


def first_existing(patterns: list[str]) -> str | None:
    for pattern in patterns:
        matches = sorted(glob.glob(pattern), reverse=True)
        if matches:
            return matches[0]
    return None


def system_flags() -> list[str] | None:
    sdl_config = shutil.which("sdl2-config")
    if not sdl_config:
        return None

    flags = command_flags([sdl_config, "--cflags", "--libs"])
    if shutil.which("pkg-config"):
        try:
            flags.extend(command_flags(["pkg-config", "--cflags", "--libs", "openssl"]))
            return flags
        except subprocess.CalledProcessError:
            pass
    brew = shutil.which("brew")
    if brew:
        for formula in ("openssl@3", "openssl"):
            try:
                prefix = Path(subprocess.check_output([brew, "--prefix", formula], text=True).strip())
            except subprocess.CalledProcessError:
                continue
            if (prefix / "include" / "openssl" / "ssl.h").exists():
                flags.extend(
                    [
                        f"-I{prefix / 'include'}",
                        f"-L{prefix / 'lib'}",
                        "-lssl",
                        "-lcrypto",
                        f"-Wl,-rpath,{prefix / 'lib'}",
                    ]
                )
                return flags
    flags.extend(["-lssl", "-lcrypto"])
    return flags


def local_flags() -> list[str] | None:
    include = LOCAL_ROOT / "usr" / "include"
    sdl_include = include / "SDL2"
    lib_dirs = sorted((LOCAL_ROOT / "usr" / "lib").glob("*-linux-gnu"))
    if not sdl_include.is_dir() or not lib_dirs:
        return None

    lib_dir = lib_dirs[0]
    sdl = first_existing([
        str(lib_dir / "libSDL2-2.0.so.*.*"),
        str(lib_dir / "libSDL2.so"),
    ])
    ssl = first_existing([
        "/lib/*-linux-gnu/libssl.so.*",
        "/usr/lib/*-linux-gnu/libssl.so.*",
    ])
    crypto = first_existing([
        "/lib/*-linux-gnu/libcrypto.so.*",
        "/usr/lib/*-linux-gnu/libcrypto.so.*",
    ])
    if not sdl or not ssl or not crypto:
        return None

    system_lib_dir = Path(ssl).parent
    return [
        f"-I{sdl_include}",
        f"-I{include}",
        *(f"-I{path}" for path in sorted(include.glob("*-linux-gnu"))),
        "-D_REENTRANT",
        f"-L{lib_dir}",
        "-lSDL2",
        f"-L{system_lib_dir}",
        f"-l:{Path(ssl).name}",
        f"-l:{Path(crypto).name}",
        f"-Wl,-rpath,{lib_dir}",
    ]


def main() -> int:
    flags = system_flags() or local_flags()
    if not flags:
        print(
            "CrossVi simulator dependencies are missing. Run: "
            "python3 scripts/setup_simulator_deps.py",
            file=sys.stderr,
        )
        return 1
    print(" ".join(shlex.quote(flag) for flag in flags))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
