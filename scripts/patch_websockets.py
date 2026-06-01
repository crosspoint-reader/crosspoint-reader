"""
PlatformIO pre-build script: apply CrossPoint's WebSockets patches via
`git apply`.

The upstream links2004/WebSockets 2.7.3 pin calls the deprecated
`NetworkClient::flush()` in `WebSocketsClient::clientDisconnect`. Recent
arduino-esp32 cores tag that method `[[deprecated("Use clear() instead.")]]`,
so every release build emits a -Wdeprecated-declarations warning. The patch
in `scripts/websockets_patches/` swaps it for the non-deprecated `clear()`,
which has the same RX-discard semantics the disconnect path wants.

Unlike JPEGDEC (a git checkout), the WebSockets libdep is an extracted
registry tarball with no `.git` directory, so we locate it by its source
file instead. `git apply` itself does not require a git working tree.

Each patch's idempotency is decided by git itself:
  * `git apply --check --reverse` succeeds  -> already applied, skip
  * `git apply --check`            succeeds  -> apply
  * neither succeeds                          -> abort the build

Patches live in `scripts/websockets_patches/` as one-commit-per-fix files.
Applied in lexical order.
"""

Import("env")  # noqa: F821 (SCons-injected global)
import os
import subprocess
import sys


PATCH_DIR = os.path.join(env["PROJECT_DIR"], "scripts", "websockets_patches")  # noqa: F821

# Sentinel file used to locate the WebSockets libdep working tree. `git apply`
# is run with this directory as cwd, matching the `a/src/...` paths in the
# patch (default -p1 strips the leading component).
SENTINEL = os.path.join("src", "WebSocketsClient.cpp")


def patch_websockets(env):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return
    patches = _patch_files()
    for env_dir in os.listdir(libdeps_dir):
        ws_dir = os.path.join(libdeps_dir, env_dir, "WebSockets")
        if not os.path.isfile(os.path.join(ws_dir, SENTINEL)):
            continue
        for patch in patches:
            _apply_one(ws_dir, patch)


def _patch_files():
    if not os.path.isdir(PATCH_DIR):
        raise RuntimeError(
            "WebSockets patches missing -- aborting build (expected directory %s)"
            % PATCH_DIR
        )
    patches = sorted(
        os.path.join(PATCH_DIR, name)
        for name in os.listdir(PATCH_DIR)
        if name.endswith(".patch")
    )
    if not patches:
        raise RuntimeError(
            "WebSockets patches missing -- aborting build (no .patch files in %s)"
            % PATCH_DIR
        )
    return patches


def _apply_one(ws_dir, patch_path):
    name = os.path.basename(patch_path)
    if _git_apply_succeeds(ws_dir, patch_path, reverse=True):
        return
    if not _git_apply_succeeds(ws_dir, patch_path, reverse=False):
        # Not applied, not appliable -- the libdep source has diverged from
        # what the patch expects. Don't write a half-patched file.
        result = subprocess.run(
            ["git", "apply", "--check", patch_path],
            cwd=ws_dir,
            capture_output=True,
            text=True,
        )
        sys.stderr.write(
            "ERROR: WebSockets patch %s does not apply cleanly:\n%s%s\n"
            % (name, result.stdout, result.stderr)
        )
        raise SystemExit(1)
    subprocess.run(["git", "apply", patch_path], cwd=ws_dir, check=True)
    print("Applied WebSockets patch: %s" % name)


def _git_apply_succeeds(ws_dir, patch_path, *, reverse):
    cmd = ["git", "apply", "--check"]
    if reverse:
        cmd.append("--reverse")
    cmd.append(patch_path)
    return subprocess.run(
        cmd, cwd=ws_dir, capture_output=True, text=True
    ).returncode == 0


patch_websockets(env)  # noqa: F821
