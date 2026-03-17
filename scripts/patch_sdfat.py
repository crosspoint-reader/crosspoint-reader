"""
PlatformIO pre-build script: patch SdFat library to support dot-prefixed directories.

SdFat's FatFile::openNext() and the LFN FatFile::open() both skip any directory
entry whose 8.3 short filename starts with '.'.  The intent is to hide the '.'
and '..' meta-entries that FAT creates inside every subdirectory, but the check
is too broad: it also discards entries for user-created dot-prefixed directories
like '.sleep' when the host OS stores the SFN with a literal leading dot.

The fix narrows the check to only match the literal '.' and '..' entries
(". " and ".. " in the 11-byte SFN field) so that other dot-prefixed entries
are resolved normally through the LFN comparison path.

Both patches are applied idempotently so it is safe to run on every build.
"""

Import("env")
import os


def patch_sdfat(env):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return
    for env_dir in os.listdir(libdeps_dir):
        sdfat_fatlib = os.path.join(libdeps_dir, env_dir, "SdFat", "src", "FatLib")
        if os.path.isdir(sdfat_fatlib):
            _patch_open_next(os.path.join(sdfat_fatlib, "FatFile.cpp"))
            _patch_lfn_open(os.path.join(sdfat_fatlib, "FatFileLFN.cpp"))


DOT_CHECK = "(dir->name[0] == '.' && (dir->name[1] == ' ' || (dir->name[1] == '.' && dir->name[2] == ' ')))"


def _patch_open_next(filepath):
    """Patch FatFile::openNext() to only skip literal '.' and '..' entries."""
    MARKER = "// CrossPoint patch: narrow dot check to literal '.' and '..'"
    if not os.path.isfile(filepath):
        return
    with open(filepath, "r") as f:
        content = f.read()

    if MARKER in content:
        return  # already patched

    OLD = """\
    // skip empty slot or '.' or '..'
    if (dir->name[0] == '.' || dir->name[0] == FAT_NAME_DELETED) {
      lfnOrd = 0;
    } else if (isFatFileOrSubdir(dir)) {"""

    NEW = """\
    // skip empty slot or '.' or '..'
    """ + MARKER + """
    if (%s || dir->name[0] == FAT_NAME_DELETED) {
      lfnOrd = 0;
    } else if (isFatFileOrSubdir(dir)) {""" % DOT_CHECK

    if OLD not in content:
        print("WARNING: SdFat openNext dot-check patch target not found in %s — library may have been updated" % filepath)
        return

    content = content.replace(OLD, NEW, 1)
    with open(filepath, "w") as f:
        f.write(content)
    print("Patched SdFat: narrow openNext dot check: %s" % filepath)


def _patch_lfn_open(filepath):
    """Patch FatFile::open(FatFile*, FatLfn_t*, oflag_t) to only skip literal '.' and '..'."""
    MARKER = "// CrossPoint patch: narrow dot check to literal '.' and '..'"
    if not os.path.isfile(filepath):
        return
    with open(filepath, "r") as f:
        content = f.read()

    if MARKER in content:
        return  # already patched

    OLD = """\
    // skip empty slot or '.' or '..'
    if (dir->name[0] == FAT_NAME_DELETED || dir->name[0] == '.') {
      lfnOrd = 0;
    } else if (isFatLongName(dir)) {"""

    NEW = """\
    // skip empty slot or '.' or '..'
    """ + MARKER + """
    if (dir->name[0] == FAT_NAME_DELETED || %s) {
      lfnOrd = 0;
    } else if (isFatLongName(dir)) {""" % DOT_CHECK

    if OLD not in content:
        print("WARNING: SdFat LFN open dot-check patch target not found in %s — library may have been updated" % filepath)
        return

    content = content.replace(OLD, NEW, 1)
    with open(filepath, "w") as f:
        f.write(content)
    print("Patched SdFat: narrow LFN open dot check: %s" % filepath)


patch_sdfat(env)
