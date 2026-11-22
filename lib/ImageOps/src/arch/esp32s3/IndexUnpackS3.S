#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include <sdkconfig.h>
#endif

#include "ImageOpsConfig.h"

#if USE_SIMD_S3_PIE
    .text

// All inputs/output/masks are aligned to 16 bytes. a4 counts 16-byte packed blocks.
// q0..q7 and SAR are caller-saved. Masks remove cross-byte shift bits.
// ZIP splits by position, interleaving the high/low fields of each input
// byte. Only the low bits of each lane are live after the final split.
    .macro split high, low
    ee.vsr.32 \high, \low
    ee.vzip.8 \high, \low
    .endm

    .macro storeIndices source
    ee.andq \source, \source, q7
    ee.vst.128.ip \source, a3, 16
    .endm

    .align 4
    .global imageOpsUnpackIndex4S3
    .type imageOpsUnpackIndex4S3, @function
imageOpsUnpackIndex4S3:
    entry a1, 16
    ee.vld.128.ip q7, a5, 0
    movi a6, 4
    wsr.sar a6
    loopnez a4, .Lindex4End
        ee.vld.128.ip q6, a2, 16
        ee.vsr.32 q0, q6
        ee.andq q0, q0, q7
        ee.andq q1, q6, q7
        ee.vzip.8 q0, q1
        ee.vst.128.ip q0, a3, 16
        ee.vst.128.ip q1, a3, 16
.Lindex4End:
    retw.n
    .size imageOpsUnpackIndex4S3, .-imageOpsUnpackIndex4S3

    .align 4
    .global imageOpsUnpackIndex2S3
    .type imageOpsUnpackIndex2S3, @function
imageOpsUnpackIndex2S3:
    entry a1, 16
    ee.vld.128.ip q7, a5, 0
    loopnez a4, .Lindex2End
        ee.vld.128.ip q3, a2, 16
        ssai 4
        split q1, q3
        ssai 2
        split q0, q1
        storeIndices q0
        storeIndices q1
        split q2, q3
        storeIndices q2
        storeIndices q3
.Lindex2End:
    retw.n
    .size imageOpsUnpackIndex2S3, .-imageOpsUnpackIndex2S3

    .align 4
    .global imageOpsUnpackIndex1S3
    .type imageOpsUnpackIndex1S3, @function
imageOpsUnpackIndex1S3:
    entry a1, 16
    ee.vld.128.ip q7, a5, 0
    loopnez a4, .Lindex1End
        ee.vld.128.ip q3, a2, 16
        ssai 4
        split q1, q3
        ssai 2
        split q0, q1
        split q2, q3
        ssai 1
        split q4, q0
        storeIndices q4
        storeIndices q0
        split q4, q1
        storeIndices q4
        storeIndices q1
        split q4, q2
        storeIndices q4
        storeIndices q2
        split q4, q3
        storeIndices q4
        storeIndices q3
.Lindex1End:
    retw.n
    .size imageOpsUnpackIndex1S3, .-imageOpsUnpackIndex1S3
#endif
