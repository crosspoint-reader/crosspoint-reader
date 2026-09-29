"""
Fetch the HarfBuzz sources that lib/HarfBuzz builds.

HarfBuzz is not vendored. This downloads the pinned upstream release,
checks its SHA-256, unpacks its src/ into lib/HarfBuzz/upstream/
(git-ignored) and applies scripts/harfbuzz_patches/*.patch in lexical order.
It does nothing when that directory already holds this release with these
patches, so only the first build (or a version or patch change) downloads.

Runs as a PlatformIO pre-build script (platformio.ini) and from the host
test build (test/CMakeLists.txt). For any other build, run it once by hand:

    python3 scripts/fetch_harfbuzz.py

Set HARFBUZZ_TARBALL to a local copy of the release tarball to build
offline; it is checked against the same SHA-256.
"""

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

# Keep in step with "version" in lib/HarfBuzz/library.json.
VERSION = "14.5.0"
URL = "https://github.com/harfbuzz/harfbuzz/releases/download/%s/harfbuzz-%s.tar.xz" % (VERSION, VERSION)
SHA256 = "b7132e148358a45185c9feafd049dbaf243649d3c44414b3534d9c95d18592b9"


def fetch_harfbuzz(project_dir):
    lib_dir = os.path.join(project_dir, "lib", "HarfBuzz")
    dest = os.path.join(lib_dir, "upstream")
    stamp_path = os.path.join(dest, ".crosspoint-stamp")
    patches = _patch_files(project_dir)
    stamp = _stamp(patches)
    if _read(stamp_path) == stamp:
        return

    _check_manifest_version(lib_dir)
    staging = tempfile.mkdtemp(prefix="upstream.", dir=lib_dir)
    try:
        tarball = os.environ.get("HARFBUZZ_TARBALL") or _download(staging)
        _verify(tarball)
        _extract_src(tarball, staging)
        for patch in patches:
            _apply(project_dir, os.path.relpath(staging, project_dir), patch)
        with open(os.path.join(staging, ".crosspoint-stamp"), "w") as f:
            f.write(stamp)
        tarball_copy = os.path.join(staging, os.path.basename(URL))
        if os.path.exists(tarball_copy):
            os.remove(tarball_copy)
        shutil.rmtree(dest, ignore_errors=True)
        os.replace(staging, dest)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    print("Fetched HarfBuzz %s into %s" % (VERSION, os.path.relpath(dest, project_dir)))


def _patch_files(project_dir):
    patch_dir = os.path.join(project_dir, "scripts", "harfbuzz_patches")
    return sorted(os.path.join(patch_dir, name) for name in os.listdir(patch_dir) if name.endswith(".patch"))


def _stamp(patches):
    lines = ["harfbuzz %s %s" % (VERSION, SHA256)]
    for patch in patches:
        with open(patch, "rb") as f:
            lines.append("%s %s" % (os.path.basename(patch), hashlib.sha256(f.read()).hexdigest()))
    return "\n".join(lines) + "\n"


def _read(path):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return None


def _check_manifest_version(lib_dir):
    with open(os.path.join(lib_dir, "library.json")) as f:
        manifest_version = json.load(f)["version"]
    if manifest_version != VERSION:
        _fail("lib/HarfBuzz/library.json is version %s but fetch_harfbuzz.py pins %s" % (manifest_version, VERSION))


def _download(directory):
    path = os.path.join(directory, os.path.basename(URL))
    print("Downloading HarfBuzz %s from %s" % (VERSION, URL))
    try:
        with urllib.request.urlopen(URL, timeout=120) as response, open(path, "wb") as out:
            shutil.copyfileobj(response, out)
    except OSError as e:
        _fail("could not download %s (%s). To build offline, set HARFBUZZ_TARBALL to a local copy." % (URL, e))
    return path


def _verify(tarball):
    digest = hashlib.sha256()
    with open(tarball, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    if digest.hexdigest() != SHA256:
        _fail("%s has SHA-256 %s, expected %s" % (tarball, digest.hexdigest(), SHA256))


def _extract_src(tarball, staging):
    prefix = "harfbuzz-%s/" % VERSION
    with tarfile.open(tarball, "r:xz") as archive:
        members = []
        for member in archive.getmembers():
            if not member.name.startswith(prefix + "src/"):
                continue
            member.name = member.name[len(prefix):]
            members.append(member)
        if hasattr(tarfile, "data_filter"):
            archive.extractall(staging, members=members, filter="data")
        else:
            archive.extractall(staging, members=members)


def _apply(project_dir, directory, patch):
    command = ["git", "apply", "--directory=" + directory.replace(os.sep, "/"), patch]
    result = subprocess.run(command, cwd=project_dir, capture_output=True, text=True)
    if result.returncode != 0:
        _fail("HarfBuzz patch %s does not apply:\n%s%s" % (os.path.basename(patch), result.stdout, result.stderr))


def _fail(message):
    sys.stderr.write("ERROR: fetch_harfbuzz: %s\n" % message)
    raise SystemExit(1)


if "Import" in globals():  # PlatformIO pre-build script: SCons injects Import()
    Import("env")  # noqa: F821
    fetch_harfbuzz(env["PROJECT_DIR"])  # noqa: F821
elif __name__ == "__main__":
    fetch_harfbuzz(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
