"""
PlatformIO pre-build script: patch JPEGDEC library for progressive JPEG support.

Four patches are applied:

1. JPEGMakeHuffTables: Skip AC Huffman table construction for progressive JPEGs.
   JPEGDEC 1.8.x fails to open progressive JPEGs because JPEGMakeHuffTables()
   cannot build AC tables with 11+-bit codes (the "slow tables" path is disabled).
   Since progressive decode only uses DC coefficients, AC tables are not needed.

2. JPEGDecodeMCU_P: Guard pMCU writes against MCU_SKIP (-8).
   The non-progressive JPEGDecodeMCU checks `iMCU >= 0` before writing to pMCU,
   but JPEGDecodeMCU_P does not.  When EIGHT_BIT_GRAYSCALE mode skips chroma
   channels by passing MCU_SKIP, the unguarded write goes to a wild pointer
   (sMCUs[0xFFFFF8]) and crashes.

3. JPEGParseInfo: Force grayscale for non-interleaved progressive JPEGs.
   If a progressive JPEG contains a luminance-only scan (Y) but the header 
   defined color (YCrCb), JPEGDEC fails when trying to decode missing chroma
   components. Forcing ucSubSample=0 (grayscale) avoids this.

4. JPEGDecode: Remove forced 1/8 scaling for progressive mode.
   Upstream v1.8.4+ forces JPEG_SCALE_EIGHTH for progressive JPEGs. We remove
   this to allow high-resolution (though blocky) DC-only decodes, which our
   converter then downsamples properly to the target size.

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
            _apply_ac_table_patch(jpeg_inl)
            _apply_mcu_skip_patch(jpeg_inl)
            _apply_grayscale_patch(jpeg_inl)
            _apply_remove_forced_scale_patch(jpeg_inl)


def _apply_ac_table_patch(filepath):
    MARKER = "// CrossPoint patch: skip AC tables for progressive JPEG"
    with open(filepath, "r") as f:
        content = f.read()

    if MARKER in content:
        return  # already patched

    OLD = """\
    }
    // now do AC components (up to 4 tables of 16-bit codes)"""

    NEW = (
        "    }\n"
        "    " + MARKER + "\n"
        "    // Progressive JPEG: only DC coefficients are decoded (first scan), so AC\n"
        "    // Huffman tables are not needed.  Skip building them to avoid failing on\n"
        "    // 11+-bit AC codes that the optimized table builder cannot handle.\n"
        "    if (pJPEG->ucMode == 0xc2)\n"
        "        return 1;\n"
        "    // now do AC components (up to 4 tables of 16-bit codes)"
    )

    if OLD not in content:
        print(
            "WARNING: JPEGDEC AC table patch target not found in %s — "
            "library may have been updated" % filepath
        )
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

    NEW_DC = (
        "        " + MARKER + "\n"
        "        if (iMCU >= 0)\n"
        "            pMCU[0] = (short)*iDCPredictor; // store in MCU[0]\n"
        "    }\n"
        "    // Now get the other 63 AC coefficients"
    )

    if OLD_DC not in content:
        print(
            "WARNING: JPEGDEC MCU_SKIP patch target not found in %s — "
            "library may have been updated" % filepath
        )
        return

    content = content.replace(OLD_DC, NEW_DC, 1)

    # Patch 2: Guard the successive approximation pMCU[0] write.
    OLD_SA = """\
                pMCU[0] |= iPositive;
            }
            goto mcu_done; // that's it"""

    NEW_SA = (
        "                if (iMCU >= 0)\n"
        "                    pMCU[0] |= iPositive;\n"
        "            }\n"
        "            goto mcu_done; // that's it"
    )

    if OLD_SA in content:
        content = content.replace(OLD_SA, NEW_SA, 1)

    with open(filepath, "w") as f:
        f.write(content)
    print("Patched JPEGDEC: guard pMCU writes for MCU_SKIP in JPEGDecodeMCU_P: %s" % filepath)


def _apply_grayscale_patch(filepath):
    MARKER = "// CrossPoint patch: force grayscale for non-interleaved progressive"
    with open(filepath, "r") as f:
        content = f.read()

    if MARKER in content:
        return

    OLD = """\
            JPEGGetSOS(pPage, &iOffset); // get Start-Of-Scan info for decoding
//        }"""

    NEW = (
        "            JPEGGetSOS(pPage, &iOffset); // get Start-Of-Scan info for decoding\n"
        "//        }\n"
        "        " + MARKER + "\n"
        "        if (pPage->ucMode == 0xc2 && pPage->ucComponentsInScan == 1) {\n"
        "            pPage->ucSubSample = 0; // Treat non-interleaved scan as grayscale\n"
        "        }"
    )

    if OLD not in content:
        print("WARNING: JPEGDEC grayscale patch target not found in %s" % filepath)
        return

    content = content.replace(OLD, NEW, 1)
    with open(filepath, "w") as f:
        f.write(content)
    print("Patched JPEGDEC: force grayscale for non-interleaved progressive: %s" % filepath)


def _apply_remove_forced_scale_patch(filepath):
    MARKER = "// CrossPoint patch: remove forced 1/8 scale for progressive mode"
    with open(filepath, "r") as f:
        content = f.read()

    # FIRST: Clean up the rogue "force 1/8 scaling" patch that might be present
    # This was likely an older version of the patch that didn't clean itself up.
    ROGUE_PATCH = """\
    // CrossPoint patch: force 1/8 scaling for progressive mode
    if (pJPEG->ucMode == 0xc2) { // progressive mode - we only decode the first scan (DC values)
        pJPEG->iOptions |= JPEG_SCALE_EIGHTH; // return 1/8 sized image
    }"""
    if ROGUE_PATCH in content:
        content = content.replace(ROGUE_PATCH, "", 1)
        # We don't return here because we still need to apply the NEW patch below
        # if it hasn't been applied yet.

    if MARKER in content:
        # If the marker is present, we've already applied the removal patch.
        # But if we just removed a rogue patch, we should write the cleaned content.
        with open(filepath, "w") as f:
            f.write(content)
        return

    # Upstream v1.8.4+ forces JPEG_SCALE_EIGHTH for progressive mode.
    # We remove this to allow high-resolution (but blocky) DC-only decodes
    # which we can then downsample properly in our converter.
    OLD = """\
    // Requested the Exif thumbnail
    if (pJPEG->ucMode == 0xc2) { // progressive mode - we only decode the first scan (DC values)
        pJPEG->iOptions |= JPEG_SCALE_EIGHTH; // return 1/8 sized image
    }"""

    NEW = (
        "    // Requested the Exif thumbnail\n"
        "    " + MARKER + "\n"
        "    // if (pJPEG->ucMode == 0xc2) { // Patched out by CrossPoint\n"
        "    //    pJPEG->iOptions |= JPEG_SCALE_EIGHTH;\n"
        "    // }"
    )

    if OLD not in content:
        # Check if the code is slightly different (without comments)
        OLD_ALT = """\
    if (pJPEG->ucMode == 0xc2) {
        pJPEG->iOptions |= JPEG_SCALE_EIGHTH;
    }"""
        if OLD_ALT in content:
            content = content.replace(OLD_ALT, NEW, 1)
        else:
            print("WARNING: JPEGDEC forced scale removal target not found in %s" % filepath)
            # If we changed content by removing rogue patch, write it anyway
            with open(filepath, "w") as f:
                f.write(content)
            return
    else:
        content = content.replace(OLD, NEW, 1)

    with open(filepath, "w") as f:
        f.write(content)
    print("Patched JPEGDEC: removed forced 1/8 scale: %s" % filepath)


# Apply patches immediately when this pre: script runs, before compilation starts.
patch_jpegdec(env)
