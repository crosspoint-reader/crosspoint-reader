"""
PlatformIO pre-build script: guard PNGdec's ESP32-S3 SIMD assembly so it
compiles on targets whose Arduino framework build lacks the esp-dsp component.

PNGdec 1.1.6 ships `s3_simd_rgb565.S` which, under `#ifdef ARDUINO_ARCH_ESP32`,
unconditionally does `#include "dsps_fft2r_platform.h"`. That header is provided
by the espressif/esp-dsp component. The ESP32-C61 Arduino framework is built
from source via custom_sdkconfig and does NOT include esp-dsp, so the raw
include is a fatal "No such file or directory" — it only ever built on C3/S3
because their prebuilt arduino-libs happen to carry the header.

JPEGDEC's equivalent `.S` files already guard the include with
`#if __has_include("dsps_fft2r_platform.h")`. This script retrofits the same
guard onto PNGdec's file so the block is skipped cleanly when the header is
absent (the SIMD path is an ESP32-S3-only optimization; C61 does not use it).

Idempotent: if the guard is already present, the file is left untouched.
"""

Import("env")  # noqa: F821 (SCons-injected global)
import os

RAW_INCLUDE = '#include "dsps_fft2r_platform.h"'
GUARDED_INCLUDE = (
    '#if __has_include("dsps_fft2r_platform.h")\n'
    '#include "dsps_fft2r_platform.h"'
)
# Matching #endif is appended after the existing include-guarded block. The
# upstream file already closes its `#ifdef ARDUINO_ARCH_ESP32` with an #endif;
# we only need to balance the __has_include we introduce, so we append one
# #endif right before that final one via a sentinel comment match.


def patch_pngdec(env):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return
    for env_dir in os.listdir(libdeps_dir):
        s_file = os.path.join(libdeps_dir, env_dir, "PNGdec", "src", "s3_simd_rgb565.S")
        if os.path.isfile(s_file):
            _patch_one(s_file)


def _patch_one(path):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()

    if "__has_include" in text:
        return  # already guarded

    if RAW_INCLUDE not in text:
        return  # upstream layout changed — leave it alone rather than corrupt it

    # Wrap the include in a __has_include guard and close it right before the
    # final trailing #endif that closes the ARDUINO_ARCH_ESP32 block.
    text = text.replace(RAW_INCLUDE, GUARDED_INCLUDE, 1)

    # Close the __has_include we opened. The file ends with the
    # `#endif // dsps_fft2r_sc16_aes3_enabled` line followed by the outer
    # `#endif` for ARDUINO_ARCH_ESP32. Insert our closing #endif after the
    # inner sc16 #endif.
    marker = "#endif // dsps_fft2r_sc16_aes3_enabled"
    if marker in text:
        text = text.replace(marker, marker + "\n#endif // __has_include dsps_fft2r_platform.h", 1)
    else:
        # Fallback: append at EOF (still balances the guard).
        text = text.rstrip() + "\n#endif // __has_include dsps_fft2r_platform.h\n"

    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print("Guarded PNGdec S3 SIMD include: %s" % path)


patch_pngdec(env)  # noqa: F821
