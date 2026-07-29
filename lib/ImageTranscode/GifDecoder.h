#pragma once

// First-frame GIF decoder (GIF87a / GIF89a) for on-device image transcoding.
//
// Why it exists: MOBI books store illustrations as GIF, but the reader only
// decodes JPEG and PNG, so GIF images silently vanish. Conversion decodes the
// frame here and re-encodes it with PngWriter (Palette8 + tRNS maps 1:1 onto a
// GIF colour table, so the transcode is lossless and needs no colour work).
//
// Animation is out of scope: the first frame is the illustration. The frame is
// emitted at its own size, never composed onto the logical screen canvas.
//
// This parses untrusted book data: every read is bounds-checked, every loop is
// bounded, and malformed input returns false rather than hanging or reading
// past the buffer.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gif {

struct Frame {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> indices;   // width*height palette indices, row-major
  std::vector<uint8_t> palette;   // 3 bytes (R,G,B) per entry, `count` entries
  uint16_t count = 0;             // palette entries, 1..256
  int16_t transparentIndex = -1;  // -1 = fully opaque
};

// Decodes the first frame of the GIF in `data` into `frame`.
// Returns false (after a LOG_ERR) on malformed input or allocation failure.
bool decodeFirstFrame(const uint8_t* data, size_t len, Frame& frame);

}  // namespace gif
