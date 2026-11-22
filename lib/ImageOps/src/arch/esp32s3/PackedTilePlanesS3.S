#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif

#include "ImageOpsConfig.h"

#if USE_SIMD_S3_PIE
    .text
    .align 4
    .global imageOpsPackedTilePlanesS3
    .type imageOpsPackedTilePlanesS3, @function

// a2=16 aligned packed bytes, a3=16 aligned plane bytes, a4=five aligned masks.
// QR registers and SAR are caller-saved.
imageOpsPackedTilePlanesS3:
    entry a1, 16
    ee.vld.128.ip q0, a2, 0
    ee.vld.128.ip q3, a4, 16
    ee.vld.128.ip q4, a4, 16
    ee.vld.128.ip q5, a4, 16
    ee.vld.128.ip q6, a4, 16
    ee.vld.128.ip q7, a4, 0
    // Reflect each 4x4 block across its opposite diagonal. This orders
    // columns left to right and puts the first row in the high bit.
    ssai 10
    ee.vsr.32 q1, q0
    ee.xorq q1, q1, q0
    ee.andq q1, q1, q3
    ee.vsl.32 q2, q1
    ee.xorq q0, q0, q1
    ee.xorq q0, q0, q2
    ssai 20
    ee.vsr.32 q1, q0
    ee.xorq q1, q1, q0
    ee.andq q1, q1, q4
    ee.vsl.32 q2, q1
    ee.xorq q0, q0, q1
    ee.xorq q0, q0, q2
    ssai 1
    ee.vsr.32 q2, q0
    ee.andq q2, q2, q5
    ee.andq q0, q0, q5
    ee.vsr.32 q1, q0
    ee.orq q0, q0, q1
    ee.andq q0, q0, q6
    ee.vsr.32 q1, q2
    ee.orq q2, q2, q1
    ee.andq q2, q2, q6
    ssai 2
    ee.vsr.32 q1, q0
    ee.orq q0, q0, q1
    ee.andq q0, q0, q7
    ee.vsr.32 q1, q2
    ee.orq q2, q2, q1
    ee.andq q2, q2, q7
    // Even words hold rows 0..3, odd words 4..7. Join the nibbles
    // and store the eight low plane bytes followed by the eight high bytes.
    ee.vunzip.32 q0, q2
    ssai 4
    ee.vsl.32 q0, q0
    ee.orq q0, q0, q2
    ee.vst.128.ip q0, a3, 0
    retw.n
    .size imageOpsPackedTilePlanesS3, .-imageOpsPackedTilePlanesS3
#endif
