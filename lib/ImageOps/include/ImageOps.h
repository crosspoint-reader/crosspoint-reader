#pragma once

#include "ImageOpsConfig.h"

#if CROSSPOINT_ACCELERATED_IMAGES
#include <cstddef>
#include <cstdint>

namespace imageops {

// One output byte (0..3) per input gray byte. x/y are absolute logical pixels.
// Exact in-place use is supported; otherwise spans must not overlap. No alignment
// or padding is required. For count == 0, pointers may be null.
// Stateless, allocation-free task code; not safe while the flash cache is off.
void ditherBayer4LevelSpan(const uint8_t* input, uint8_t* output, size_t count, int x, int y);

// Convert exactly 64 row-major bytes, each 0..3, to 16 plane bytes:
// planes[c] = low bits of column c, planes[8+c] = high bits; row 0 is bit 7.
// No alignment/padding is required. Input and output must not overlap.
// No clipping, validity masks, orientation transform or framebuffer merge.
// Caller pads partial tiles before calling and masks their output afterward.
// Stateless, allocation-free task code; not safe while the flash cache is off.
void levelTile8x8ToPlanes(const uint8_t* levels, uint8_t* planes);

// Same output as levelTile8x8ToPlanes. Each of the eight row pointers supplies
// exactly two MSB-first packed 2bpp bytes (pixels 0..3 then 4..7).
// Rows may alias each other and need not be contiguous or aligned. All eight
// pointers must be valid; output must not overlap the rows or pointer array.
// The same task/resource and caller-padding rules apply.
void packed2BitTile8x8ToPlanes(const uint8_t* const rows[8], uint8_t* planes);

// Read exactly 3*count packed R,G,B bytes and write count gray bytes using
// (77*R + 150*G + 29*B) >> 8. Input/output must not overlap; no alignment or
// padding is required. For count == 0, pointers may be null. No alpha policy.
// Stateless, allocation-free task code; not safe while the flash cache is off.
void rgb24ToGraySpan(const uint8_t* input, uint8_t* output, size_t count);

// Same span/resource contract as rgb24ToGraySpan, but input bytes are B,G,R.
void bgr24ToGraySpan(const uint8_t* input, uint8_t* output, size_t count);

enum class IndexDepth : uint8_t { Bits1 = 1, Bits2 = 2, Bits4 = 4, Bits8 = 8 };

// Read MSB-first packed indices starting at first; write exactly count byte indices.
// Input supplies through index first+count-1; no alignment/padding is required.
// Input/output must not overlap. Empty spans accept null pointers.
void unpackIndexSpan(const uint8_t* input, uint8_t* output, size_t count, IndexDepth depth, size_t first = 0);

// Write 1<<depth entries to each non-overlapping gray/alpha LUT. Palette RGB
// entries are three bytes each; when hasAlpha, alpha entries start at byte 768.
// Retained alpha keeps raw gray; otherwise gray is composited against white.
// A null palette expands indices to 0..255 gray and ignores hasAlpha.
// Outputs must not overlap the palette. Only the four named depths are valid.
// Both indexed helpers are stateless, allocation-free ordinary task code.
// Accessible buffers and enabled flash cache are required, as for the other ops.
void prepareIndexedPalette(const uint8_t* palette, IndexDepth depth, bool hasAlpha, bool retainAlpha, uint8_t* gray,
                           uint8_t* alpha);

}  // namespace imageops
#endif
