/* CrossPoint only needs miniz's low-level streaming inflate (tinfl). The
 * archive, deflate, stdio, and zlib-compatibility layers are compiled out so
 * the vendored library stays small and never touches the filesystem or clock.
 * Include this header instead of <miniz.h> so every translation unit sees the
 * same configuration. */
#pragma once

#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES

// The ESP32 mask ROM exports tinfl_* at fixed addresses via DIRECT linker
// script assignments (e.g. "tinfl_decompress = 0x...;" in the ROM .ld),
// which override object-file definitions -- without these renames the
// firmware silently binds to the ROM's 2021 build (TINFL_LESS_MEMORY, a
// different tinfl_decompressor layout) and corrupts inflate state on real
// data. Rename so the linker can never capture them. The prefix is
// crosspoint_ (NOT freeink_) so a future branch that links FreeInkBook's
// identically-renamed copy does not collide.
#define tinfl_decompress crosspoint_tinfl_decompress
#define tinfl_decompress_mem_to_heap crosspoint_tinfl_decompress_mem_to_heap
#define tinfl_decompress_mem_to_mem crosspoint_tinfl_decompress_mem_to_mem
#define tinfl_decompress_mem_to_callback crosspoint_tinfl_decompress_mem_to_callback
#define mz_crc32 crosspoint_mz_crc32
#define mz_adler32 crosspoint_mz_adler32
#define mz_free crosspoint_mz_free

// Include the vendored miniz by relative path: ESP-IDF ships a ROM miniz.h
// with the SAME include guard but a different (TINFL_LESS_MEMORY) struct
// layout -- resolving <miniz.h> through the platform include path would
// silently compile against the wrong structures.
#include "../third_party/miniz.h"
