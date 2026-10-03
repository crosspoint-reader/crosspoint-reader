"""
Pocket Library: PlatformIO pre-build script that applies our patches to the
freeink-sdk submodule (scripts/pocketlib_sdk_patches/*.patch, lexical order).
SPDX-License-Identifier: GPL-3.0-or-later

Used only by the x4pro-pocketlib envs. The patched code is fenced in macros
only those envs define, so a stock env built from the same (patched) tree
compiles exactly what upstream does.

Idempotent, decided by git itself:
  * `git apply --check --reverse` succeeds -> already applied, skip
  * `git apply --check`           succeeds -> apply
  * neither                                -> abort the build (the SDK moved;
                                              refresh the patch)
"""

Import("env")  # noqa: F821 (SCons-injected global)
import os
import subprocess

PROJECT_DIR = env["PROJECT_DIR"]  # noqa: F821
SDK_DIR = os.path.join(PROJECT_DIR, "freeink-sdk")
PATCH_DIR = os.path.join(PROJECT_DIR, "scripts", "pocketlib_sdk_patches")


def _git(args):
    return subprocess.run(["git", "-C", SDK_DIR] + args, capture_output=True, text=True)


def apply_patches():
    patches = sorted(p for p in os.listdir(PATCH_DIR) if p.endswith(".patch"))
    if not patches:
        raise RuntimeError("Pocket Library SDK patches missing in %s" % PATCH_DIR)
    for name in patches:
        path = os.path.join(PATCH_DIR, name)
        if _git(["apply", "--check", "--reverse", path]).returncode == 0:
            continue
        if _git(["apply", "--check", path]).returncode != 0:
            raise RuntimeError(
                "Pocket Library: %s no longer applies to freeink-sdk; refresh it" % name
            )
        result = _git(["apply", path])
        if result.returncode != 0:
            raise RuntimeError("Pocket Library: applying %s failed: %s" % (name, result.stderr))
        print("Pocket Library: applied %s to freeink-sdk" % name)


apply_patches()
