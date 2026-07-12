"""
PlatformIO pre-build script: patch JPEGDEC library for progressive JPEG support.

Two patches are applied:

1. JPEGMakeHuffTables: Skip AC Huffman table construction for progressive JPEGs.
   JPEGDEC 1.8.x fails to open progressive JPEGs because JPEGMakeHuffTables()
   cannot build AC tables with 11+-bit codes (the "slow tables" path is disabled).
   Since progressive decode only uses DC coefficients, AC tables are not needed.

2. JPEGDecodeMCU_P: Guard pMCU writes against MCU_SKIP (-8).
   The non-progressive JPEGDecodeMCU checks `iMCU >= 0` before writing to pMCU,
   but JPEGDecodeMCU_P does not.  When EIGHT_BIT_GRAYSCALE mode skips chroma
   channels by passing MCU_SKIP, the unguarded write goes to a wild pointer
   (sMCUs[0xFFFFF8]) and crashes.

Both patches are applied idempotently so it is safe to run on every build.
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
            _apply_ac_table_patch(jpeg_inl)
            _apply_mcu_skip_patch(jpeg_inl)

def _apply_ac_table_patch(filepath):
    MARKER = "// CrossPoint patch: skip AC tables for progressive JPEG"
    with open(filepath, "r") as f:
        content = f.read()

    if MARKER in content:
        return  # already patched

    OLD = """\
    }
    // now do AC components (up to 4 tables of 16-bit codes)"""

    NEW = """\
    }
    """ + MARKER + """
    // Progressive JPEG: only DC coefficients are decoded (first scan), so AC
    // Huffman tables are not needed.  Skip building them to avoid failing on
    // 11+-bit AC codes that the optimized table builder cannot handle.
    if (pJPEG->ucMode == 0xc2)
        return 1;
    // now do AC components (up to 4 tables of 16-bit codes)"""

    if OLD not in content:
        print("WARNING: JPEGDEC AC table patch target not found in %s — library may have been updated" % filepath)
        return

    content = content.replace(OLD, NEW, 1)
    with open(filepath, "w") as f:
        f.write(content)
    print("Patched JPEGDEC: skip AC tables for progressive JPEG: %s" % filepath)

def _apply_mcu_skip_patch(filepath):
    MARKER = "// CrossPoint patch: guard pMCU write for MCU_SKIP"
    with open(filepath, "r") as f:
        content = f.read()

    if MARKER in content:
        return  # already patched

    # Patch 1: Guard the unconditional pMCU[0] write in JPEGDecodeMCU_P.
    # This is the DC coefficient store that crashes when iMCU = MCU_SKIP (-8).
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

    if OLD_DC not in content:
        print("WARNING: JPEGDEC MCU_SKIP patch target not found in %s — library may have been updated" % filepath)
        return

    content = content.replace(OLD_DC, NEW_DC, 1)

    # Patch 2: Guard the successive approximation pMCU[0] write.
    # This path is taken on subsequent scans (cApproxBitsHigh != 0), which we
    # don't normally hit (we only decode first scan), but guard it for safety.
    OLD_SA = """\
                pMCU[0] |= iPositive;
            }
            goto mcu_done; // that's it"""

    NEW_SA = """\
                if (iMCU >= 0)
                    pMCU[0] |= iPositive;
            }
            goto mcu_done; // that's it"""

    if OLD_SA in content:
        content = content.replace(OLD_SA, NEW_SA, 1)

    with open(filepath, "w") as f:
        f.write(content)
    print("Patched JPEGDEC: guard pMCU writes for MCU_SKIP in JPEGDecodeMCU_P: %s" % filepath)

# Apply patches immediately when this pre: script runs, before compilation starts.
# Previously used env.AddPreAction("buildprog", ...) which deferred patching until
# the link step — after the library was already compiled from unpatched source.
patch_jpegdec(env)
