#!/usr/bin/env python3
"""Install simulator build headers locally when system packages are unavailable."""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / ".cache" / "simulator-deps"
PACKAGES = CACHE / "packages"
DESTINATION = CACHE / "root"


def command_succeeds(command: list[str]) -> bool:
    return (
        subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode
        == 0
    )


def homebrew_openssl_available() -> bool:
    brew = shutil.which("brew")
    if not brew:
        return False
    for formula in ("openssl@3", "openssl"):
        try:
            prefix = Path(subprocess.check_output([brew, "--prefix", formula], text=True).strip())
        except subprocess.CalledProcessError:
            continue
        if (prefix / "include" / "openssl" / "ssl.h").exists():
            return True
    return False


def system_deps_available() -> bool:
    if not shutil.which("sdl2-config"):
        return False
    has_openssl = Path("/usr/include/openssl/ssl.h").exists()
    pkg_config = shutil.which("pkg-config")
    if pkg_config:
        has_openssl = has_openssl or command_succeeds([pkg_config, "--exists", "openssl"])
    return has_openssl or homebrew_openssl_available()


def local_deps_available() -> bool:
    include = DESTINATION / "usr" / "include"
    has_headers = (include / "SDL2" / "SDL.h").exists() and (include / "openssl" / "ssl.h").exists()
    has_sdl_runtime = any((DESTINATION / "usr" / "lib").glob("*-linux-gnu/libSDL2-2.0.so.*.*"))
    return has_headers and has_sdl_runtime


def main() -> int:
    if system_deps_available():
        print("Simulator dependencies are already installed system-wide.")
        return 0
    if local_deps_available():
        print(f"Simulator dependencies are already available in {CACHE}")
        return 0
    if not shutil.which("apt-get") or not shutil.which("dpkg-deb"):
        print(
            "Automatic local setup currently supports Debian/Ubuntu/Linux Mint only.\n"
            "Install SDL2 and OpenSSL development packages for your OS, then retry.",
            file=sys.stderr,
        )
        return 1

    PACKAGES.mkdir(parents=True, exist_ok=True)
    DESTINATION.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["apt-get", "download", "libsdl2-dev", "libsdl2-2.0-0", "libssl-dev"],
        cwd=PACKAGES,
        check=True,
    )
    archives = sorted(PACKAGES.glob("*.deb"))
    if not archives:
        print("apt-get did not download the required packages.", file=sys.stderr)
        return 1
    for archive in archives:
        subprocess.run(["dpkg-deb", "-x", str(archive), str(DESTINATION)], check=True)

    print(f"Simulator headers installed locally in {CACHE}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
