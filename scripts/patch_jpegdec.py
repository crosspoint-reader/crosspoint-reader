"""
PlatformIO pre-build script: patch JPEGDEC library for progressive JPEG support.

Two patches are applied:

1. JPEGDecodeMCU_P: Guard pMCU writes against MCU_SKIP (-8) by redirecting to a safe buffer.
   Upstream commit 8628297 guarded the DC coefficient write (pMCU[0]) but not the
   AC coefficient writes at indices 1-63. Redirecting pMCU to sMCUs[0] when MCU_SKIP
   is active prevents wild pointer crashes while remaining safe as sMCUs[0] is
   already guarded for DC writes.

2. JPEGParseInfo: Force grayscale for non-interleaved progressive JPEGs.
   If a progressive JPEG contains a luminance-only scan (Y) but the header 
   defined color (YCrCb), JPEGDEC fails when trying to decode missing chroma
   components. Forcing ucSubSample=0 (grayscale) avoids this.

Note: Previous patches for AC Huffman table construction and 1/8 scale forcing
for progressive mode have been removed as they are now handled upstream in
JPEGDEC v1.8.x.

All patches are applied idempotently so it is safe to run on every build.
"""

Import("env")
import os


def patch_jpegdec(env):
    # Find the JPEGDEC library in libdeps
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return
    for env_dir in os.listdir(libdeps_dir):
        jpeg_inl = os.path.join(libdeps_dir, env_dir, "JPEGDEC", "src", "jpeg.inl")
        if os.path.isfile(jpeg_inl):
            _apply_mcu_skip_pointer_fix(jpeg_inl)
            _apply_grayscale_patch(jpeg_inl)


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


def _apply_grayscale_patch(filepath):
    MARKER = "// CrossPoint patch: force grayscale for non-interleaved progressive"
    with open(filepath, "r") as f:
        content = f.read()

    if MARKER in content:
        return

    OLD = """\
            JPEGGetSOS(pPage, &iOffset); // get Start-Of-Scan info for decoding
//        }"""

    NEW = """\
            JPEGGetSOS(pPage, &iOffset); // get Start-Of-Scan info for decoding
//        }
        """ + MARKER + """
        if (pPage->ucMode == 0xc2 && pPage->ucComponentsInScan == 1) {
            pPage->ucSubSample = 0; // Treat non-interleaved scan as grayscale
        }"""

    if OLD not in content:
        print("WARNING: JPEGDEC grayscale patch target not found in %s" % filepath)
        return

    content = content.replace(OLD, NEW, 1)
    with open(filepath, "w") as f:
        f.write(content)
    print("Patched JPEGDEC: force grayscale for non-interleaved progressive: %s" % filepath)


# Apply patches immediately when this pre: script runs, before compilation starts.
patch_jpegdec(env)
