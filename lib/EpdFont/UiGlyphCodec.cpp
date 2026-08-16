#include "UiGlyphCodec.h"

#include <cstring>

namespace {

// P(ink | ctx) in 1/65536 units; ctx = W<<2 | N<<1 | NW (1 = ink, out of
// bounds = 0). Trained on all Pretendard 8/10/12pt regular glyphs (38,658
// glyphs) with scripts kept alongside the perf investigation; the model
// generalizes across CJK fonts because it captures stroke continuation, not
// font-specific shapes. Probabilities are clamped away from 0/65536 so the
// range coder always keeps a non-empty interval for both symbols.
constexpr uint16_t kCtx3ProbInk[8] = {4592, 1519, 50050, 12833, 53543, 18376, 64013, 59153};

constexpr uint32_t kTopValue = 1u << 24;

// LZMA-style binary range coder. The canonical first output byte is always 0
// (a carry landing pad); we omit it on the wire and re-synthesize it in the
// decoder. A carry that would reach it makes encode() fail -> caller stores
// the glyph raw. Trailing zero bytes of the 5-byte flush are truncated; the
// decoder feeds zero for reads past the end, which reproduces them exactly.
struct RangeEncoder {
  uint8_t* out;
  uint16_t capacity;
  uint16_t pos = 0;
  uint64_t low = 0;
  uint32_t range = 0xFFFFFFFFu;
  uint8_t cache = 0;
  uint32_t cacheSize = 1;
  bool firstByte = true;
  bool failed = false;

  void putByte(uint8_t b) {
    if (firstByte) {
      firstByte = false;
      if (b != 0) failed = true;  // carry reached the landing pad byte
      return;                     // canonical leading zero: not stored
    }
    if (pos >= capacity) {
      failed = true;
      return;
    }
    out[pos++] = b;
  }

  void shiftLow() {
    if (static_cast<uint32_t>(low) < 0xFF000000u || (low >> 32) != 0) {
      uint8_t carry = static_cast<uint8_t>(low >> 32);
      uint8_t b = cache;
      do {
        putByte(static_cast<uint8_t>(b + carry));
        b = 0xFF;
      } while (--cacheSize != 0);
      cache = static_cast<uint8_t>(low >> 24);
    }
    cacheSize++;
    low = (static_cast<uint32_t>(low)) << 8;
  }

  void encodeBit(uint16_t probInk, int bit) {
    const uint32_t bound = (range >> 16) * probInk;
    if (bit) {
      range = bound;
    } else {
      low += bound;
      range -= bound;
    }
    while (range < kTopValue) {
      shiftLow();
      range <<= 8;
    }
  }

  uint16_t flush() {
    for (int i = 0; i < 5; i++) shiftLow();
    if (failed) return 0;
    while (pos > 0 && out[pos - 1] == 0) pos--;  // decoder zero-fills past end
    return pos;
  }
};

struct RangeDecoder {
  const uint8_t* in;
  uint16_t len;
  uint16_t pos = 0;
  uint32_t range = 0xFFFFFFFFu;
  uint32_t code = 0;

  uint8_t nextByte() { return pos < len ? in[pos++] : 0; }

  void init() {
    // The canonical leading zero byte is implicit (not stored), so the first
    // of the five init bytes is synthesized as 0 by starting from code = 0.
    for (int i = 0; i < 4; i++) code = (code << 8) | nextByte();
  }

  int decodeBit(uint16_t probInk) {
    const uint32_t bound = (range >> 16) * probInk;
    int bit;
    if (code < bound) {
      bit = 1;
      range = bound;
    } else {
      bit = 0;
      code -= bound;
      range -= bound;
    }
    while (range < kTopValue) {
      range <<= 8;
      code = (code << 8) | nextByte();
    }
    return bit;
  }
};

inline int packedBit(const uint8_t* packed, uint32_t pixelIndex) {
  return (packed[pixelIndex >> 3] >> (7 - (pixelIndex & 7))) & 1;
}

}  // namespace

bool UiGlyphCodec::sourcePixel(const uint8_t* src, bool srcIs2Bit, uint32_t pixelIndex) {
  if (srcIs2Bit) {
    const uint8_t byte = src[pixelIndex >> 2];
    return ((byte >> ((3 - (pixelIndex & 3)) * 2)) & 0x3) != 0;
  }
  return packedBit(src, pixelIndex) != 0;
}

void UiGlyphCodec::convertTo1Bit(const uint8_t* src, bool srcIs2Bit, uint8_t width, uint8_t height, uint8_t* dst) {
  const uint32_t pixelCount = static_cast<uint32_t>(width) * height;
  memset(dst, 0, packed1BitBytes(pixelCount));
  for (uint32_t i = 0; i < pixelCount; i++) {
    if (sourcePixel(src, srcIs2Bit, i)) {
      dst[i >> 3] |= 0x80 >> (i & 7);
    }
  }
}

uint16_t UiGlyphCodec::encode(const uint8_t* src, bool srcIs2Bit, uint8_t width, uint8_t height, uint8_t* dst,
                              uint16_t dstCapacity) {
  const uint32_t pixelCount = static_cast<uint32_t>(width) * height;
  if (pixelCount == 0) return 0;

  RangeEncoder rc{dst, dstCapacity};
  uint32_t i = 0;
  for (uint8_t y = 0; y < height; y++) {
    for (uint8_t x = 0; x < width; x++, i++) {
      const int w = (x > 0) ? sourcePixel(src, srcIs2Bit, i - 1) : 0;
      const int n = (y > 0) ? sourcePixel(src, srcIs2Bit, i - width) : 0;
      const int nw = (x > 0 && y > 0) ? sourcePixel(src, srcIs2Bit, i - width - 1) : 0;
      const uint16_t p = kCtx3ProbInk[(w << 2) | (n << 1) | nw];
      rc.encodeBit(p, sourcePixel(src, srcIs2Bit, i) ? 1 : 0);
      if (rc.failed) return 0;
    }
  }
  return rc.flush();
}

void UiGlyphCodec::decode(const uint8_t* src, uint16_t srcLen, uint8_t width, uint8_t height, uint8_t* dst) {
  const uint32_t pixelCount = static_cast<uint32_t>(width) * height;
  memset(dst, 0, packed1BitBytes(pixelCount));
  if (pixelCount == 0) return;

  RangeDecoder rc{src, srcLen};
  rc.init();
  uint32_t i = 0;
  for (uint8_t y = 0; y < height; y++) {
    for (uint8_t x = 0; x < width; x++, i++) {
      // Context reads from already-decoded output bits — no extra state.
      const int w = (x > 0) ? packedBit(dst, i - 1) : 0;
      const int n = (y > 0) ? packedBit(dst, i - width) : 0;
      const int nw = (x > 0 && y > 0) ? packedBit(dst, i - width - 1) : 0;
      const uint16_t p = kCtx3ProbInk[(w << 2) | (n << 1) | nw];
      if (rc.decodeBit(p)) {
        dst[i >> 3] |= 0x80 >> (i & 7);
      }
    }
  }
}
