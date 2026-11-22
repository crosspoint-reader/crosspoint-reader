#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include <sdkconfig.h>
#endif

#include "ImageOpsConfig.h"

#if USE_SIMD_S3_PIE
    .text

// Three aligned word loads supply four RGB24/BGR24 pixels. Fourth bytes in the
// register-only RGBA layout are ignored; no RGBA bytes are written to memory.
    .macro rgbFour target, offset
    l32i a6, a2, \offset
    l32i a7, a2, (\offset + 4)
    l32i a8, a2, (\offset + 8)
    ee.movi.32.q \target, a6, 0
    ssai 24
    src a9, a7, a6
    ee.movi.32.q \target, a9, 1
    ssai 16
    src a9, a8, a7
    ee.movi.32.q \target, a9, 2
    srli a9, a8, 8
    ee.movi.32.q \target, a9, 3
    .endm

    .align 4
    .global imageOpsRgb24GrayS3Blocks
    .type imageOpsRgb24GrayS3Blocks, @function

// a2=4-byte-aligned RGB24/BGR24, a3=4-byte-aligned gray, a4=eight-pixel blocks,
// a5=four aligned coefficient vectors selecting channel order.
// Reads 24 bytes, writes eight per block.
// QR/SAR caller-saved.
imageOpsRgb24GrayS3Blocks:
    entry a1, 16
    ee.vld.128.ip q4, a5, 16
    ee.vld.128.ip q5, a5, 16
    ee.vld.128.ip q6, a5, 16
    ee.vld.128.ip q7, a5, 0
    loopnez a4, .LpackedRgbEnd
        rgbFour q0, 0
        rgbFour q1, 12
        addi a2, a2, 24
        // Two unzips group eight lanes of each channel into halves.
        ee.vunzip.8 q0, q1
        ee.vunzip.8 q0, q1
        ee.zero.q q2
        ee.zero.q q3
        ee.vzip.8 q0, q2
        ee.vzip.8 q1, q3
        // Widened channel order: first q0, green q2, last q1.
        ssai 0
        ee.vmul.s16 q0, q0, q4
        ee.vmul.s16 q1, q1, q6
        ee.vadds.s16 q0, q0, q1
        ee.vmul.s16 q1, q2, q5
        ee.vadds.s16 q0, q0, q1
        ssai 8
        ee.vmul.s16 q0, q0, q7
        ee.vadds.s16 q0, q0, q2
        ee.vunzip.8 q0, q3
        ee.movi.32.a q0, a7, 0
        ee.movi.32.a q0, a8, 1
        s32i a7, a3, 0
        s32i a8, a3, 4
        addi a3, a3, 8
.LpackedRgbEnd:
    retw.n
    .size imageOpsRgb24GrayS3Blocks, .-imageOpsRgb24GrayS3Blocks
#endif
