// Pocket Library: the one file added to the vendored libwebp. Defining
// HAVE_CONFIG_H (library.json, lib/zim/CMakeLists.txt) makes libwebp read
// this instead of probing the compiler, so no SSE/NEON code paths are
// selected: those files were removed (the ESP32-S3 has neither), and host
// tests then build the same C code the device runs.
#ifndef WEBP_WEBP_CONFIG_H_
#define WEBP_WEBP_CONFIG_H_
#endif
