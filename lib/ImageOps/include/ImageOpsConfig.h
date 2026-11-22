#pragma once

#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include <sdkconfig.h>
#endif

// Enable optimized image paths by default on ESP32-S3 only. A global 0/1
// override applies on all targets; enabled non-S3 builds use scalar kernels.
#ifndef CROSSPOINT_ACCELERATED_IMAGES
#if defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3
#define CROSSPOINT_ACCELERATED_IMAGES 1
#else
#define CROSSPOINT_ACCELERATED_IMAGES 0
#endif
#endif

#if CROSSPOINT_ACCELERATED_IMAGES != 0 && CROSSPOINT_ACCELERATED_IMAGES != 1
#error "CROSSPOINT_ACCELERATED_IMAGES must be 0 or 1"
#endif

#if defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3
#define USE_SIMD_S3_PIE CROSSPOINT_ACCELERATED_IMAGES
#else
#define USE_SIMD_S3_PIE 0
#endif
