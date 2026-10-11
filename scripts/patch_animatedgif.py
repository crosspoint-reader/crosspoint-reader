"""
PlatformIO pre-build script: patch AnimatedGIF to honor CrossPoint's MAX_WIDTH override.

The pinned AnimatedGIF release hard-codes MAX_WIDTH inside the header, which
rejects wide static GIFs before our own scale-to-fit logic runs. We patch that
single define block in-place so the build flag from platformio.ini can take
effect. The replacement is idempotent.
"""

Import("env")  # noqa: F821 (SCons-injected global)
import os
import re
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

MAX_WIDTH_DEFINE_RE = re.compile(r"^\s*#\s*define\s+MAX_WIDTH\b", re.MULTILINE)
MAX_WIDTH_GUARD_RE = re.compile(
    r"^\s*#\s*(?:ifndef\s+MAX_WIDTH\b|if\s+!\s*defined\s*\(?\s*MAX_WIDTH\s*\)?)",
    re.MULTILINE,
)


def _patch_header(header_path):
    with open(header_path, "r", encoding="utf-8") as file:
        text = file.read()

    max_width_define = MAX_WIDTH_DEFINE_RE.search(text)
    max_width_guard = MAX_WIDTH_GUARD_RE.search(text)
    if NEW_SNIPPET in text or (
        max_width_define
        and max_width_guard
        and max_width_guard.start() < max_width_define.start()
    ):
        return True

    if OLD_SNIPPET not in text:
        if not max_width_define:
            sys.stderr.write(
                "WARNING: AnimatedGIF no longer defines MAX_WIDTH in %s\n"
                % header_path
            )
            return True
        sys.stderr.write(
            "ERROR: AnimatedGIF has an unguarded MAX_WIDTH definition in %s\n"
            % header_path
        )
        raise SystemExit(1)

    with open(header_path, "w", encoding="utf-8") as file:
        file.write(text.replace(OLD_SNIPPET, NEW_SNIPPET, 1))

    print("Patched AnimatedGIF MAX_WIDTH guard")
    return True


def patch_animatedgif(env, require_found=False):
    header_path = os.path.join(
        env.subst("$PROJECT_LIBDEPS_DIR"),
        env.subst("$PIOENV"),
        "AnimatedGIF",
        "src",
        "AnimatedGIF.h",
    )
    patched_any = os.path.isfile(header_path) and _patch_header(header_path)

    if require_found and not patched_any:
        sys.stderr.write("ERROR: AnimatedGIF dependency was not found to patch\n")
        raise SystemExit(1)

    return patched_any


def patch_animatedgif_before_link(source, target, env):
    patch_animatedgif(env, require_found=True)


patch_animatedgif(env, require_found=False)  # noqa: F821
env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", patch_animatedgif_before_link)  # noqa: F821
