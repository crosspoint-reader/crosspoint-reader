#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

// The on-disk .pxc pixel-cache format, shared by the writer (PixelCache, in
// converters/) and the reader (ImageBlock, in blocks/). It lives in neither:
// the converter would otherwise include the block header just for these two
// constants, which points the dependency backwards for what is only a file
// format.
//
// A cached image, in the .pxc file and in a PSRAM render-cache slot alike:
// uint16 width, uint16 height, then ((width + 3) / 4) * height bytes of 2-bpp
// pixels, packed 4 per byte, MSB first, row-major.
constexpr size_t PXC_HEADER_BYTES = 4;
// 96,000 B of payload is a full-screen 800x480 image at 2 bpp. Anything larger
// cannot be a page image on this hardware and stays on the streaming path
// rather than sizing the cache for it.
constexpr size_t PXC_MAX_PAYLOAD_BYTES = 96000;

#ifdef CROSSPOINT_PSRAM_IMAGE_CACHE
// One-shot .pxc donation, called by PixelCache when a decode buffered the whole
// image in PSRAM: rather than have the ~12 render passes that follow re-read the
// file the decode just wrote, the buffer is handed straight to the render cache.
// `pxcImage` must be one heap_caps_malloc block (MALLOC_CAP_SPIRAM) holding
// exactly `byteCount` bytes in the format above. On true the cache takes
// ownership and frees the block when the slot is evicted; on false the caller
// keeps it. Caller must hold the RenderLock (see the cache's threading note in
// ImageBlock.cpp, which is where this is implemented).
bool adoptPxcImage(const std::string& cachePath, uint8_t* pxcImage, size_t byteCount);
#endif
