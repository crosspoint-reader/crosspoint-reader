"""
PlatformIO pre-build script: patch AnimatedGIF to honor CrossPoint's MAX_WIDTH override.

The pinned AnimatedGIF release hard-codes MAX_WIDTH inside the header, which
rejects wide static GIFs before our own scale-to-fit logic runs. We patch that
single define block in-place so the build flag from platformio.ini can take
effect. The replacement is idempotent.
"""

Import("env")  # noqa: F821 (SCons-injected global)
import os
import sys


OLD_SNIPPET = """#define MAX_COLORS 256
#ifdef __LINUX__
#define MAX_WIDTH 2048
#else
#define MAX_WIDTH 480
#endif // __LINUX__
"""

NEW_SNIPPET = """#define MAX_COLORS 256
#ifndef MAX_WIDTH
#ifdef __LINUX__
#define MAX_WIDTH 2048
#else
#define MAX_WIDTH 480
#endif // __LINUX__
#endif // MAX_WIDTH
"""


def _patch_header(header_path):
    with open(header_path, "r", encoding="utf-8") as file:
        text = file.read()

    if NEW_SNIPPET in text:
        return True

    if OLD_SNIPPET not in text:
        sys.stderr.write(
            "ERROR: AnimatedGIF MAX_WIDTH block not found in %s\n" % header_path
        )
        raise SystemExit(1)

    with open(header_path, "w", encoding="utf-8") as file:
        file.write(text.replace(OLD_SNIPPET, NEW_SNIPPET, 1))

    print("Patched AnimatedGIF MAX_WIDTH guard")
    return True


def patch_animatedgif(env, require_found=False):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    patched_any = False

    if os.path.isdir(libdeps_dir):
        for env_dir in os.listdir(libdeps_dir):
            header_path = os.path.join(
                libdeps_dir,
                env_dir,
                "AnimatedGIF",
                "src",
                "AnimatedGIF.h",
            )
            if os.path.isfile(header_path):
                patched_any = _patch_header(header_path) or patched_any

    if require_found and not patched_any:
        sys.stderr.write("ERROR: AnimatedGIF dependency was not found to patch\n")
        raise SystemExit(1)

    return patched_any


def patch_animatedgif_before_link(source, target, env):
    patch_animatedgif(env, require_found=True)


patch_animatedgif(env, require_found=False)  # noqa: F821
env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", patch_animatedgif_before_link)  # noqa: F821