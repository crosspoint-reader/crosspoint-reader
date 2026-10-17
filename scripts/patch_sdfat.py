"""
PlatformIO pre-build hook: apply the SdFat patches in scripts/sdfat_patches/ to the
SdFat copy this env builds (.pio/libdeps/<env>/SdFat).

Ported from crosspoint-reader PR #3685 ("perf: keep FAT sectors in a separate SdFat
cache", Sung-jin Brian Hong / @serialx). The two patches are theirs, verbatim:

  0001  FsCache: invalidate the cache after a failed sector read. With SHARED_SPI
        (how every SPI board here mounts the card), SdSpiCard::readSectors() fills
        the cache buffer and only then sends CMD12; if CMD12 fails, the cache still
        names the previous sector while holding the new one's bytes. A later
        CACHE_FOR_WRITE of that sector writes those bytes back -- to both FAT copies
        when it is a FAT sector.
  0002  SdFatConfig: let -DUSE_SEPARATE_FAT_CACHE=1 (platformio.ini) override the
        ARM-only default, so FAT lookups stop evicting the file/directory sector.

One more is OURS, not upstream's (see its header):

  0003  FatPartition: with the separate FAT cache, sync the FAT before the
        directory sector, the order the single shared cache gave by eviction, so a
        power cut mid-sync cannot leave an entry pointing at clusters the FAT
        still marks free.

And one mostly from crosspoint-reader PR #3969 ("fix: keep SdFat's free cluster
search moving forward after a delete", Tuan Q. Nguyen / @martinqnguyen), theirs
verbatim apart from the init() hunks:

  0004  FatPartition: freeing a cluster no longer pulls the free cluster search
        back down to it; the search wraps once instead. Each O_TRUNC rewrite of a
        file low on the card made the next new file read the FAT from there to
        the free space, some 2,000 sectors on a 16 GB card half full. Ours on
        top: a FAT32 mount starts the search at the FSInfo next-free hint,
        range-checked, instead of at cluster 2.

Their safety gates come along too: SdFat must be exactly the release the patches were
reviewed against, and every file a patch touches must hash to that release's bytes or
to the reviewed patched bytes. Everything is checked before anything is written, and
an already-patched file is left untouched so incremental builds recompile nothing.
Bumping SdFat therefore means re-reviewing every patch and re-deriving PATCHES.

What differs from upstream:
  * A pre: hook that looks the copy up in this env's libdeps dir, in the shape of
    patch_uzlib.py / patch_wolfssl.py, rather than a post: hook walking the
    resolved lib builders. PlatformIO installs lib_deps before it starts SCons, so
    the copy is already there; and running before pioarduino's SDK-only pass
    blanks lib_deps means that pass needs no special case. A second, detached copy
    (SdFat@x.y.z) fails the build, since which one gets compiled is then a guess.
  * Line endings. The patch text is normalised to LF and handed to git on stdin: a
    Windows checkout under core.autocrlf=true has CRLF .patch files, which do not
    apply to the LF registry sources. And git runs with system/global config off
    and core.autocrlf=false, or Git for Windows' system-wide autocrlf plus SdFat's
    own `* text=auto` .gitattributes rewrite the WHOLE patched file as CRLF. Both
    were reproduced on Windows; the hash gate is what catches the second.
  * The set of .patch files must match PATCHES exactly, so a new patch cannot be
    dropped in without its hashes and silently skipped.

Runs as a PlatformIO pre: hook and standalone:
  python scripts/patch_sdfat.py              # every .pio/libdeps/*/SdFat
  python scripts/patch_sdfat.py DIR [DIR..]  # the given SdFat directories
"""

import glob
import hashlib
import os
import subprocess
import sys

try:
    Import("env")  # noqa: F821 -- provided by PlatformIO when run as a build hook
    PIO_ENV = env  # noqa: F821
    PROJECT_DIR = env["PROJECT_DIR"]  # noqa: F821
except (NameError, Exception):  # not under PlatformIO -> standalone invocation
    PIO_ENV = None
    PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PATCH_DIR = os.path.join(PROJECT_DIR, "scripts", "sdfat_patches")

# Must match the greiman/SdFat pin in platformio.ini.
SDFAT_VERSION = "2.3.1"

# patch -> (file it changes, sha256 of SdFat 2.3.1's file, sha256 once patched).
# Applied in this order.
PATCHES = (
    (
        "0001-invalidate-failed-cache-fill.patch",
        "src/common/FsCache.cpp",
        "ba4f99dd660c7c6a747b20abcf11354379aa689115d1ee1ad1cf9577706a67bb",
        "f46a551f97c674ab00c9c25385af7c370a776725f17e14502fcc357858f583da",
    ),
    (
        "0002-allow-separate-fat-cache-override.patch",
        "src/SdFatConfig.h",
        "7889975cad262158e1623c873730210c41c55c2198830d668952b82e588ff7cc",
        "32104db82acc857b70c7fe20740afbffa9f7dee0a7c685c106febfa989fdbe77",
    ),
    (
        "0003-sync-fat-cache-before-data-cache.patch",
        "src/FatLib/FatPartition.h",
        "fb91c81beac2779a0d4b08e886a5fefdf8062b8ad422ccec4e178ae4e88b72e5",
        "ad9d6a745aa8c91c8f006a61b2a6296e8dd89a1d05f1b335446e371ec541efcc",
    ),
    (
        "0004-keep-free-cluster-search-moving-forward.patch",
        "src/FatLib/FatPartition.cpp",
        "36163b3dc4ee985da9a6c6fb99b635fb5b63255d35fa5b3ccc1778db1534c0fe",
        "46847407b29354adb72be0448d60fccd4eb23e01fdb2061ee231aa58ff108396",
    ),
)


def _fail(message):
    sys.stderr.write("ERROR: %s\n" % message)
    raise SystemExit(1)


def _sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def _read_patch(name):
    with open(os.path.join(PATCH_DIR, name), "rb") as f:
        return f.read().replace(b"\r\n", b"\n")


def _git_apply(sdfat_dir, patch_text, *args):
    git_env = dict(os.environ)
    # A wrapper or hook could otherwise point git at another repository or inject config.
    for key in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE",
                "GIT_CONFIG_PARAMETERS", "GIT_CONFIG_COUNT"):
        git_env.pop(key, None)
    git_env.update(
        # .pio/libdeps lives inside this project's work tree. Without the ceiling, git
        # would run as that repo and take its config and attributes along.
        GIT_CEILING_DIRECTORIES=os.path.dirname(os.path.abspath(sdfat_dir)),
        GIT_CONFIG_NOSYSTEM="1",
        GIT_CONFIG_GLOBAL=os.devnull,
    )
    return subprocess.run(
        ["git", "-c", "core.autocrlf=false", "apply"] + list(args) + ["-"],
        cwd=sdfat_dir,
        env=git_env,
        input=patch_text,
        capture_output=True,
    )


def _check_patch_set():
    if not os.path.isdir(PATCH_DIR):
        _fail("SdFat patches missing -- aborting build (expected directory %s)" % PATCH_DIR)
    found = sorted(n for n in os.listdir(PATCH_DIR) if n.endswith(".patch"))
    expected = sorted(p[0] for p in PATCHES)
    if found != expected:
        _fail(
            "%s holds %s, but patch_sdfat.py has reviewed hashes for %s. A patch needs an "
            "entry in PATCHES before it can be applied." % (PATCH_DIR, found, expected)
        )


def patch_sdfat(sdfat_dir):
    """Bring one SdFat copy to the patched state, or fail before writing anything."""
    properties = os.path.join(sdfat_dir, "library.properties")
    if not os.path.isfile(properties):
        _fail("%s is not an SdFat library (no library.properties)" % sdfat_dir)
    with open(properties, encoding="utf-8", errors="replace") as f:
        if ("version=" + SDFAT_VERSION) not in f.read().splitlines():
            _fail(
                "%s is not SdFat %s, the release scripts/sdfat_patches/ was reviewed "
                "against. Check the greiman/SdFat pin in platformio.ini."
                % (sdfat_dir, SDFAT_VERSION)
            )

    pending = []
    for name, relative, upstream_sha, patched_sha in PATCHES:
        target = os.path.join(sdfat_dir, relative)
        if not os.path.isfile(target):
            _fail("SdFat patch %s: %s is missing from %s" % (name, relative, sdfat_dir))
        current = _sha256(target)
        if current == patched_sha:
            continue
        if current != upstream_sha:
            _fail(
                "SdFat patch %s: %s is neither SdFat %s's file nor the patched one "
                "(sha256 %s). Delete %s and rebuild to re-download it."
                % (name, relative, SDFAT_VERSION, current, sdfat_dir)
            )
        patch_text = _read_patch(name)
        result = _git_apply(sdfat_dir, patch_text, "--check")
        if result.returncode != 0:
            _fail(
                "SdFat patch %s does not apply cleanly to %s:\n%s"
                % (name, sdfat_dir, result.stderr.decode("utf-8", "replace"))
            )
        pending.append((name, target, patched_sha, patch_text))

    for name, target, patched_sha, patch_text in pending:
        result = _git_apply(sdfat_dir, patch_text)
        if result.returncode != 0:
            _fail("SdFat patch %s failed:\n%s"
                  % (name, result.stderr.decode("utf-8", "replace")))
        if _sha256(target) != patched_sha:
            _fail(
                "SdFat patch %s left %s with unexpected bytes (a line-ending rewrite?). "
                "Delete %s and rebuild." % (name, target, sdfat_dir)
            )
        print("Applied SdFat patch: %s (%s)" % (name, sdfat_dir))


def _pio_targets():
    # `pio run -t clean` runs pre: hooks without installing dependencies first, and
    # compiles nothing, so a fresh checkout has no copy yet and needs none.
    if PIO_ENV.IsCleanTarget():
        return []
    libdeps = os.path.join(PIO_ENV.subst("$PROJECT_LIBDEPS_DIR"), PIO_ENV["PIOENV"])
    copies = []
    if os.path.isdir(libdeps):
        copies = sorted(n for n in os.listdir(libdeps)
                        if n == "SdFat" or n.startswith("SdFat@"))
    if len(copies) > 1:
        _fail(
            "%s holds more than one SdFat (%s), so which one gets compiled is up to the "
            "dependency finder. Delete them and rebuild; the pin installs a single copy."
            % (libdeps, ", ".join(copies))
        )
    if not copies:
        # env:bench trims lib_deps to SaxParser, so it legitimately has no SdFat. An env
        # that pins SdFat but has no copy would build an unpatched one from elsewhere.
        lib_deps = PIO_ENV.GetProjectOption("lib_deps", [])
        if any("greiman/sdfat" in dep.lower() for dep in lib_deps):
            _fail("platformio.ini pins greiman/SdFat, but %s has no copy of it" % libdeps)
        return []
    return [os.path.join(libdeps, copies[0])]


def _standalone_targets(argv):
    if argv:
        return [os.path.abspath(path) for path in argv]
    return sorted(glob.glob(os.path.join(PROJECT_DIR, ".pio", "libdeps", "*", "SdFat")))


def main():
    _check_patch_set()
    targets = _pio_targets() if PIO_ENV is not None else _standalone_targets(sys.argv[1:])
    for sdfat_dir in targets:
        patch_sdfat(sdfat_dir)


main()
