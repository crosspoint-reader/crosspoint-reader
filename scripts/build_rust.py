"""Build the Rust UI framework and link it into the firmware.

PlatformIO runs this as a `pre:` script. It compiles the `crosspoint_rs`
staticlib for the target matching the current environment and appends the
resulting archive to the link line.

`crosspoint_rs` statically contains `cpui`, so only one archive is
built and linked.

Targets, keyed off the environment's MCU:
    <host>        simulator envs, std, stable toolchain
    esp32c3       riscv32imc-unknown-none-elf, no_std, stable toolchain
    esp32s3       xtensa-esp32s3-none-elf, no_std, `esp` toolchain, build-std

The Xtensa target is tier 3 and ships no prebuilt core/alloc, so it is built
from source via `-Z build-std`. That requires the esp-rs toolchain (`espup
install`) with the `rust-src` component.
"""

import os
import platform
import subprocess
import sys

Import("env")  # noqa: F821 - injected by PlatformIO's SConscript runner

WORKSPACE_MANIFEST = "Cargo.toml"
STATICLIB = "crosspoint_rs"
BUILD_TIMEOUT_SECONDS = 600


def host_triple():
    """Rust triple for the machine running the build (simulator envs)."""
    machine = platform.machine()
    system = platform.system()

    if system == "Darwin":
        return "aarch64-apple-darwin" if machine == "arm64" else "x86_64-apple-darwin"
    if system == "Linux":
        return "aarch64-unknown-linux-gnu" if machine == "aarch64" else "x86_64-unknown-linux-gnu"
    if system == "Windows":
        return "x86_64-pc-windows-msvc"

    raise RuntimeError("Unsupported host platform for the Rust build: {} {}".format(system, machine))


def target_for_env(env):
    """Return (triple, toolchain, build_std) for the active PlatformIO env.

    toolchain is None for the default stable toolchain; build_std is True when
    core/alloc must be compiled from source.
    """
    if "native" in env.get("PIOPLATFORM", ""):
        return host_triple(), None, False

    mcu = env.BoardConfig().get("build.mcu", "")

    if mcu == "esp32s3":
        return "xtensa-esp32s3-none-elf", "esp", True
    if mcu == "esp32c3":
        return "riscv32imc-unknown-none-elf", None, False

    raise RuntimeError(
        "No Rust target mapped for MCU '{}'. Add it to scripts/build_rust.py "
        "before building this environment.".format(mcu)
    )


def build(project_dir, triple, toolchain, build_std):
    """Run cargo and return the directory holding the built archive."""
    command = ["cargo"]
    if toolchain:
        command.append("+" + toolchain)
    command += ["build", "--release", "--package", STATICLIB, "--target", triple]
    if build_std:
        command += ["-Z", "build-std=core,alloc"]

    print("[rust] {}".format(" ".join(command)))

    result = subprocess.run(
        command,
        cwd=project_dir,
        timeout=BUILD_TIMEOUT_SECONDS,
        capture_output=True,
        text=True,
    )

    if result.returncode != 0:
        # cargo writes diagnostics to stderr; surface them or the failure is opaque.
        sys.stderr.write(result.stderr)
        raise RuntimeError("cargo build failed for target {}".format(triple))

    # Warnings still matter even on success.
    if result.stderr.strip():
        print(result.stderr.strip())

    return os.path.join(project_dir, "target", triple, "release")


def main(env):
    project_dir = env.subst("$PROJECT_DIR")

    if not os.path.exists(os.path.join(project_dir, WORKSPACE_MANIFEST)):
        raise RuntimeError("Rust workspace manifest not found at project root")

    triple, toolchain, build_std = target_for_env(env)
    artifact_dir = build(project_dir, triple, toolchain, build_std)

    archive = os.path.join(artifact_dir, "lib{}.a".format(STATICLIB))
    if not os.path.exists(archive):
        raise RuntimeError("Expected Rust archive not produced: {}".format(archive))

    env.Append(LIBPATH=[artifact_dir], LIBS=[STATICLIB])

    # Relink whenever the archive changes, otherwise SCons caches a stale binary.
    env.Depends("$BUILD_DIR/${PROGNAME}$PROGSUFFIX", archive)

    print("[rust] linking {} ({})".format(archive, triple))


main(env)  # noqa: F821
