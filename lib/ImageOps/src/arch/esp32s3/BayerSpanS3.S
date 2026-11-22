#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif

#include "ImageOpsConfig.h"

#if USE_SIMD_S3_PIE
    .text
    .align 4
    .global imageOpsBayerSpanS3Blocks
    .type imageOpsBayerSpanS3Blocks, @function

// a2=input, a3=output, a4=16-pixel blocks, a5=Bayer offsets, a6=constants.
// All addresses must be 16-byte aligned. QR registers and SAR are caller-saved.
imageOpsBayerSpanS3Blocks:
    entry a1, 16
    ee.vld.128.ip q1, a5, 0
    ee.vld.128.ip q2, a6, 16
    ee.vld.128.ip q3, a6, 0
    movi a7, 6
    wsr.sar a7
    loopnez a4, .LimageOpsBayerEnd
        ee.vld.128.ip q0, a2, 16
        ee.xorq q0, q0, q2
        ee.vadds.s8 q0, q0, q1
        ee.xorq q0, q0, q2
        // Shift each word; the mask removes inter-byte bits and sign extension.
        ee.vsr.32 q0, q0
        ee.andq q0, q0, q3
        ee.vst.128.ip q0, a3, 16
.LimageOpsBayerEnd:
    retw.n
    .size imageOpsBayerSpanS3Blocks, .-imageOpsBayerSpanS3Blocks
#endif
