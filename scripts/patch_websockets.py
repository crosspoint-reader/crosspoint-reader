"""
PlatformIO pre-build script: apply CrossPoint's WebSockets patches.

The upstream links2004/WebSockets 2.7.3 pin calls the deprecated
`NetworkClient::flush()` in `WebSocketsClient::clientDisconnect`. Recent
arduino-esp32 cores tag that method `[[deprecated("Use clear() instead.")]]`,
so every release build emits a -Wdeprecated-declarations warning. The patch
in `scripts/websockets_patches/` swaps it for the non-deprecated `clear()`,
which has the same RX-discard semantics the disconnect path wants.

We use GNU `patch`, not `git apply`, on purpose. The WebSockets libdep lives
under `.pio/libdeps/<env>/WebSockets`, which sits *inside* the project's own
git work tree (`.pio` is gitignored, but git still owns the path). `git apply`
resolves against that enclosing repo rather than the libdep, so it silently
no-ops -- both the forward and reverse `--check` "succeed" without touching
the file. (JPEGDEC dodges this only because it is installed as its own git
checkout with a private `.git`.) GNU `patch` ignores git entirely and edits
the file in place, which is what we need here.

Patches are read with `-i` so stdin stays free; stdin is fed from /dev/null
so any prompt defaults to "no". That yields clean three-state detection:
  * reverse dry-run succeeds  -> already applied, skip
  * forward dry-run succeeds  -> apply
  * neither succeeds          -> source diverged, abort the build

Patches live in `scripts/websockets_patches/` as one-commit-per-fix files.
Applied in lexical order.
"""

Import("env")  # noqa: F821 (SCons-injected global)
import os
import subprocess
import sys


PATCH_DIR = os.path.join(env["PROJECT_DIR"], "scripts", "websockets_patches")  # noqa: F821

# Sentinel file used to locate the WebSockets libdep working tree. `patch` is
# run with this directory as cwd, matching the `a/src/...` paths in the patch
# (default -p1 strips the leading component).
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
    if _patch_succeeds(ws_dir, patch_path, dry_run=True, reverse=True):
        return  # already applied
    if not _patch_succeeds(ws_dir, patch_path, dry_run=True, reverse=False):
        # Not applied, not appliable -- the libdep source has diverged from
        # what the patch expects. Don't write a half-patched file.
        result = _run_patch(ws_dir, patch_path, dry_run=True, reverse=False)
        sys.stderr.write(
            "ERROR: WebSockets patch %s does not apply cleanly:\n%s%s\n"
            % (name, result.stdout, result.stderr)
        )
        raise SystemExit(1)
    if not _patch_succeeds(ws_dir, patch_path, dry_run=False, reverse=False):
        sys.stderr.write("ERROR: WebSockets patch %s failed to apply\n" % name)
        raise SystemExit(1)
    print("Applied WebSockets patch: %s" % name)


def _patch_succeeds(ws_dir, patch_path, *, dry_run, reverse):
    return _run_patch(ws_dir, patch_path, dry_run=dry_run, reverse=reverse).returncode == 0


def _run_patch(ws_dir, patch_path, *, dry_run, reverse):
    # -i reads the patch from a file so stdin is free; feeding stdin from
    # /dev/null makes any GNU patch prompt default to "no" (skip), which is
    # exactly the non-destructive behaviour we want for the dry-run probes.
    cmd = ["patch", "-p1", "-i", patch_path]
    if dry_run:
        cmd.append("--dry-run")
    if reverse:
        cmd.append("--reverse")
    with open(os.devnull, "rb") as devnull:
        return subprocess.run(
            cmd, cwd=ws_dir, stdin=devnull, capture_output=True, text=True
        )


patch_websockets(env)  # noqa: F821
