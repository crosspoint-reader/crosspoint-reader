"""
PlatformIO pre-build script: apply CrossPoint's JPEGDEC patches via `git apply`.

The upstream JPEGDEC pin still has the wild-pointer + DC-write bugs in
JPEGDecodeMCU_P that surface when EIGHT_BIT_GRAYSCALE decodes a 3-component
progressive JPEG (each Y MCU drags two MCU_SKIP calls behind it for Cb/Cr).
The patches in `scripts/jpegdec_patches/` carry the fix; this script applies
each one against the libdep working tree.

Each patch's idempotency is decided by git itself:
  * `git apply --check --reverse` succeeds  -> already applied, skip
  * `git apply --check`            succeeds  -> apply
  * neither succeeds                          -> abort the build

Patches live in `scripts/jpegdec_patches/` as one-commit-per-fix files
(see the file headers for context). Applied in lexical order.
"""

Import("env")
import os
import subprocess
import sys


PATCH_DIR = os.path.join(env["PROJECT_DIR"], "scripts", "jpegdec_patches")


def patch_jpegdec(env):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return
    patches = _patch_files()
    if not patches:
        return
    for env_dir in os.listdir(libdeps_dir):
        jpeg_dir = os.path.join(libdeps_dir, env_dir, "JPEGDEC")
        if not os.path.isdir(os.path.join(jpeg_dir, ".git")):
            continue
        for patch in patches:
            _apply_one(jpeg_dir, patch)


def _patch_files():
    if not os.path.isdir(PATCH_DIR):
        return []
    return sorted(
        os.path.join(PATCH_DIR, name)
        for name in os.listdir(PATCH_DIR)
        if name.endswith(".patch")
    )


def _apply_one(jpeg_dir, patch_path):
    name = os.path.basename(patch_path)
    if _git_apply_succeeds(jpeg_dir, patch_path, reverse=True):
        return
    if not _git_apply_succeeds(jpeg_dir, patch_path, reverse=False):
        # Not applied, not appliable -- the libdep source has diverged from
        # what the patch expects. Don't write a half-patched file.
        result = subprocess.run(
            ["git", "apply", "--check", patch_path],
            cwd=jpeg_dir,
            capture_output=True,
            text=True,
        )
        sys.stderr.write(
            "ERROR: JPEGDEC patch %s does not apply cleanly:\n%s%s\n"
            % (name, result.stdout, result.stderr)
        )
        raise SystemExit(1)
    subprocess.run(["git", "apply", patch_path], cwd=jpeg_dir, check=True)
    print("Applied JPEGDEC patch: %s" % name)


def _git_apply_succeeds(jpeg_dir, patch_path, *, reverse):
    cmd = ["git", "apply", "--check"]
    if reverse:
        cmd.append("--reverse")
    cmd.append(patch_path)
    return subprocess.run(
        cmd, cwd=jpeg_dir, capture_output=True, text=True
    ).returncode == 0


patch_jpegdec(env)
