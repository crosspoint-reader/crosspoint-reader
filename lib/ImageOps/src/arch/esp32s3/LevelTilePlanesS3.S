#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif

#include "ImageOpsConfig.h"

#if USE_SIMD_S3_PIE
    .text
    .align 4
    .global imageOpsLevelTilePlanesS3
    .type imageOpsLevelTilePlanesS3, @function

// a2=64 aligned row-major levels, a3=16 aligned plane bytes, a4=six aligned masks.
// Keep transposed columns in registers; store only eight low and eight high bytes.
// QR registers and SAR are caller-saved.
imageOpsLevelTilePlanesS3:
    entry a1, 16
    // Read forward, but reverse the row order in registers so row 0 packs
    // into bit 7 without reversing the finished planes.
    ee.vld.h.64.ip q3, a2, 8
    ee.vld.l.64.ip q3, a2, 8
    ee.vld.h.64.ip q2, a2, 8
    ee.vld.l.64.ip q2, a2, 8
    ee.vld.h.64.ip q1, a2, 8
    ee.vld.l.64.ip q1, a2, 8
    ee.vld.h.64.ip q0, a2, 8
    ee.vld.l.64.ip q0, a2, 8
    ee.vzip.8 q0, q1
    ee.vzip.8 q2, q3
    ee.vzip.16 q0, q2
    ee.vzip.16 q1, q3
    ee.vzip.8 q0, q1
    ee.vzip.8 q2, q3
    ee.vld.128.ip q4, a4, 16
    ee.vld.128.ip q5, a4, 16
    ssai 6
    ee.vsr.32 q6, q0
    ee.orq q0, q0, q6
    ee.andq q0, q0, q4
    ee.vsr.32 q6, q1
    ee.orq q1, q1, q6
    ee.andq q1, q1, q4
    ee.vsr.32 q6, q2
    ee.orq q2, q2, q6
    ee.andq q2, q2, q4
    ee.vsr.32 q6, q3
    ee.orq q3, q3, q6
    ee.andq q3, q3, q4
    ssai 12
    ee.vsr.32 q6, q0
    ee.orq q0, q0, q6
    ee.andq q0, q0, q5
    ee.vsr.32 q6, q1
    ee.orq q1, q1, q6
    ee.andq q1, q1, q5
    ee.vsr.32 q6, q2
    ee.orq q2, q2, q6
    ee.andq q2, q2, q5
    ee.vsr.32 q6, q3
    ee.orq q3, q3, q6
    ee.andq q3, q3, q5
    ee.vunzip.8 q0, q1
    ee.vunzip.8 q2, q3
    ee.vunzip.8 q0, q2
    ee.vld.128.ip q3, a4, 16
    ee.vld.128.ip q4, a4, 16
    ee.vld.128.ip q5, a4, 16
    ee.vld.128.ip q7, a4, 0
    ssai 1
    ee.vsr.32 q1, q0
    ee.andq q1, q1, q3
    ee.andq q0, q0, q3
    ee.vsr.32 q6, q0
    ee.orq q0, q0, q6
    ee.andq q0, q0, q4
    ee.vsr.32 q6, q1
    ee.orq q1, q1, q6
    ee.andq q1, q1, q4
    ssai 2
    ee.vsr.32 q6, q0
    ee.orq q0, q0, q6
    ee.andq q0, q0, q5
    ee.vsr.32 q6, q1
    ee.orq q1, q1, q6
    ee.andq q1, q1, q5
    ssai 4
    ee.vsr.32 q6, q0
    ee.orq q0, q0, q6
    ee.andq q0, q0, q7
    ee.vsr.32 q6, q1
    ee.orq q1, q1, q6
    ee.andq q1, q1, q7
    ee.vunzip.8 q0, q1
    ee.vst.128.ip q0, a3, 0
    retw.n
    .size imageOpsLevelTilePlanesS3, .-imageOpsLevelTilePlanesS3
#endif
