"""
PlatformIO pre-build script: patch JPEGDEC for MCU_SKIP wild pointer crash.

Problem:
  JPEGDecodeMCU_P computes pMCU = &sMCUs[iMCU & 0xffffff].  When iMCU is
  MCU_SKIP (-8), the bitmask produces index 0xFFFFF8 (16 777 208), creating a
  pointer ~33 MB past the 392-entry sMCUs array.  If the progressive JPEG's
  first scan includes AC coefficients (iScanEnd > 0), the AC decode loop writes
  through this wild pointer and crashes with a store-access fault.

  Upstream commit 8628297 guarded the DC coefficient write (pMCU[0]) but not the
  AC coefficient writes at indices 1-63.

Fix:
  Redirect pMCU to sMCUs[0] when MCU_SKIP is active.  Writes to sMCUs[1..63]
  are harmless: for JPEG_SCALE_EIGHTH only sMCUs[0] is read for output, and
  the DC write at sMCUs[0] is already guarded by the existing `if (iMCU >= 0)`
  check.

Applied idempotently — safe to run on every build.
"""

Import("env")
import os


def patch_jpegdec(env):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return
    for env_dir in os.listdir(libdeps_dir):
        jpeg_inl = os.path.join(libdeps_dir, env_dir, "JPEGDEC", "src", "jpeg.inl")
        if os.path.isfile(jpeg_inl):
            _apply_mcu_skip_pointer_fix(jpeg_inl)


def _apply_mcu_skip_pointer_fix(filepath):
    MARKER = "// CrossPoint patch: safe pMCU for MCU_SKIP"
    with open(filepath, "r") as f:
        content = f.read()

    if MARKER in content:
        return  # already patched

    # The wild-pointer line in JPEGDecodeMCU_P:
    OLD = "    signed short *pMCU = &pJPEG->sMCUs[iMCU & 0xffffff];"

    NEW = (
        "    " + MARKER + "\n"
        "    signed short *pMCU = (iMCU < 0) ? pJPEG->sMCUs\n"
        "                                     : &pJPEG->sMCUs[iMCU & 0xffffff];"
    )

    if OLD not in content:
        print(
            "WARNING: JPEGDEC MCU_SKIP pointer patch target not found in %s "
            "— library may have been updated" % filepath
        )
        return

    content = content.replace(OLD, NEW, 1)
    with open(filepath, "w") as f:
        f.write(content)
    print("Patched JPEGDEC: safe pMCU for MCU_SKIP in JPEGDecodeMCU_P: %s" % filepath)


# Run immediately at script import time (before compilation).
patch_jpegdec(env)
