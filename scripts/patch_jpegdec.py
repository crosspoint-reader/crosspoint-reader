"""
PlatformIO pre-build script: patch JPEGDEC for safe MCU_SKIP handling.

When iMCU is MCU_SKIP (-8), JPEGDecodeMCU_P computes pMCU as
&sMCUs[iMCU & 0xffffff] = &sMCUs[0xFFFFF8], a wild pointer ~33 MB past
the array. EIGHT_BIT_GRAYSCALE decoding of a 3-component progressive
JPEG calls JPEGDecodeMCU_P with MCU_SKIP twice per Y MCU (Cb then Cr),
so every MCU exercises the wild pointer.

Two patches, both required:

1. Redirect pMCU to &sMCUs[0] when iMCU < 0. Without this, the AC
   decode loop (`pMCU[iIndex] = ...`) store-faults on any progressive
   JPEG whose first scan carries AC coefficients (iScanEnd > 0).

2. Guard the two pMCU[0] DC writes with `if (iMCU >= 0)`. Without
   this, patch 1 just relocates the corruption: chroma-skip DC writes
   land at sMCUs[0] and clobber the freshly-decoded Y DC, producing
   all-black output for progressive JPEGs at JPEG_SCALE_EIGHTH grayscale.

The AC loop body (`pMCU[iIndex] = ...` writes and matching reads) is
not separately guarded. It is unreachable on the JPEG_SCALE_EIGHTH
DC-only first-scan path, and guarding the writes without also skipping
bit consumption would desync the bitstream for the next MCU.

Both patches are idempotent.
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
            _apply_dc_write_guards(jpeg_inl)


def _apply_mcu_skip_pointer_fix(filepath):
    MARKER = "// CrossPoint patch: safe pMCU for MCU_SKIP"
    with open(filepath, "r") as f:
        content = f.read()

    if MARKER in content:
        return

    OLD = "    signed short *pMCU = &pJPEG->sMCUs[iMCU & 0xffffff];"

    NEW = (
        "    " + MARKER + "\n"
        "    signed short *pMCU = (iMCU < 0) ? pJPEG->sMCUs\n"
        "                                     : &pJPEG->sMCUs[iMCU & 0xffffff];"
    )

    if OLD not in content:
        print(
            "WARNING: JPEGDEC MCU_SKIP pointer patch target not found in %s "
            "-- library may have been updated" % filepath
        )
        return

    content = content.replace(OLD, NEW, 1)
    with open(filepath, "w") as f:
        f.write(content)
    print("Patched JPEGDEC: safe pMCU for MCU_SKIP in JPEGDecodeMCU_P: %s" % filepath)


def _apply_dc_write_guards(filepath):
    MARKER = "// CrossPoint patch: guard pMCU DC writes for MCU_SKIP"
    with open(filepath, "r") as f:
        content = f.read()

    if MARKER in content:
        return

    OLD_DC = """\
        pMCU[0] = (short)*iDCPredictor; // store in MCU[0]
    }
    // Now get the other 63 AC coefficients"""

    NEW_DC = """\
        """ + MARKER + """
        if (iMCU >= 0)
            pMCU[0] = (short)*iDCPredictor; // store in MCU[0]
    }
    // Now get the other 63 AC coefficients"""

    OLD_SA = """\
                pMCU[0] |= iPositive;
            }
            goto mcu_done; // that's it"""

    NEW_SA = """\
                if (iMCU >= 0)
                    pMCU[0] |= iPositive;
            }
            goto mcu_done; // that's it"""

    if OLD_DC not in content:
        print(
            "WARNING: JPEGDEC DC write guard target not found in %s "
            "-- library may have been updated" % filepath
        )
        return

    content = content.replace(OLD_DC, NEW_DC, 1)
    if OLD_SA in content:
        content = content.replace(OLD_SA, NEW_SA, 1)
    else:
        print(
            "WARNING: JPEGDEC successive-approximation DC guard target not found in %s "
            "-- continuing without that half of the patch" % filepath
        )

    with open(filepath, "w") as f:
        f.write(content)
    print("Patched JPEGDEC: guard pMCU[0] DC writes for MCU_SKIP in JPEGDecodeMCU_P: %s" % filepath)


patch_jpegdec(env)
