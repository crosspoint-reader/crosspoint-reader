"""Fails the firmware build when the TTF engine (lib/EpdFont/ttf) uses floating point (spec 5.2).

The C3 has no FPU: float or double maths would pull in libgcc's slow soft-float routines, which the engine
was designed to avoid. The linker map cannot show this. It names only the first object that pulled in each
archive member, and other code (SdFat, the battery monitor) already pulls in the double routines. So this
script reads the engine's own objects instead: any undefined soft-float or libm symbol in one of them fails
the build.

PlatformIO runs it after every link (platformio.ini: extra_scripts = post:scripts/check_no_float.py).
By hand:
    python scripts/check_no_float.py <path to nm> <build dir>
"""

import os
import re
import subprocess
import sys

# libgcc's soft-float entry points: __addsf3, __muldf3, __eqsf2, __fixdfsi, __floatsisf, __extendsfdf2, ...
SOFT_FLOAT = re.compile(
    r"^__(?:(?:add|sub|mul|div|neg|eq|ne|ge|gt|le|lt|unord|cmp)(?:sf|df)\d"
    r"|(?:fix|fixuns)(?:sf|df)(?:si|di)"
    r"|float(?:un)?(?:si|di)(?:sf|df)"
    r"|extendsfdf2|truncdfsf2)$"
)
LIBM = re.compile(
    r"^(?:sqrt|cbrt|hypot|pow|exp|exp2|log|log2|log10|sin|cos|tan|asin|acos|atan|atan2"
    r"|floor|ceil|round|lround|trunc|fmod|ldexp|frexp|modf)f?$"
)
ENGINE_DIR = "/EpdFont/ttf/"


def engine_objects(build_dir):
    """Every object compiled from lib/EpdFont/ttf, its third_party folder included."""
    for root, _, files in os.walk(build_dir):
        path = root.replace("\\", "/") + "/"
        if ENGINE_DIR not in path:
            continue
        for name in files:
            if name.endswith(".o"):
                yield os.path.join(root, name)


def float_symbols(nm, obj, env=None):
    out = subprocess.run([nm, "-u", obj], capture_output=True, text=True, check=True, env=env).stdout
    symbols = {line.split()[-1] for line in out.splitlines() if line.strip()}
    return sorted(s for s in symbols if SOFT_FLOAT.match(s) or LIBM.match(s))


def check(nm, build_dir, env=None):
    """Prints the verdict; returns the number of offending objects."""
    objects = sorted(engine_objects(build_dir))
    bad = 0
    for obj in objects:
        found = float_symbols(nm, obj, env)
        if found:
            bad += 1
            print(f"check_no_float: {os.path.relpath(obj, build_dir)} uses floating point: {', '.join(found)}")
    if not objects:
        print("check_no_float: this build has no TTF engine objects; nothing to check")
        return 0
    if not bad:
        print(f"check_no_float: {len(objects)} TTF engine objects, no floating point")
    return bad


try:
    Import("env")  # noqa: F821 -- provided by PlatformIO when it runs this as an extra script
except NameError:
    env = None

if env is not None:

    def _after_link(target, source, env):  # noqa: ARG001 -- SCons action signature
        nm = env.WhereIs(env.subst("$CC").replace("gcc", "nm"))
        if not nm:
            print("check_no_float: cannot find the toolchain's nm")
            return 1
        return 1 if check(nm, env.subst("$BUILD_DIR"), env["ENV"]) else 0

    env.AddPostAction(  # noqa: F821
        "$BUILD_DIR/${PROGNAME}.elf", env.VerboseAction(_after_link, "Checking the TTF engine for floating point")
    )
elif __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(1 if check(sys.argv[1], sys.argv[2]) else 0)
