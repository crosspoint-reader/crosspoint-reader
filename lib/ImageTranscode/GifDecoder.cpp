#include "GifDecoder.h"

#include <Logging.h>
#include <Memory.h>

#include <cstring>

namespace gif {
namespace {

// A frame this big cannot be shown on an 800x480 panel, but the cap exists to
// stop a 13-byte header from asking for a gigabyte: width and height are both
// 16-bit, so the product alone reaches 4 gigapixels.
constexpr uint32_t MAX_PIXELS = 16u * 1024 * 1024;
constexpr uint16_t MAX_CODES = 4096;  // LZW dictionary ceiling (12-bit codes)

uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

// Cursor over the file. Bounds checks subtract instead of adding, because
// `pos + n` wraps on a hostile length and silently passes.
struct Reader {
  const uint8_t* d;
  size_t len;
  size_t pos;

  bool has(size_t n) const { return pos <= len && n <= len - pos; }
  bool byte(uint8_t& v) {
    if (!has(1)) return false;
    v = d[pos++];
    return true;
  }
  // Returns a pointer to `n` bytes inside the buffer, or nullptr if they are
  // not all there. No copy: `data` outlives the decode.
  const uint8_t* take(size_t n) {
    if (!has(n)) return nullptr;
    const uint8_t* p = d + pos;
    pos += n;
    return p;
  }
};

// Walks a sub-block chain (length byte + payload, a zero length terminates) and
// hands back the first sub-block in place, which is where every extension we
// care about keeps its fields. Bounded: every iteration consumes at least the
// length byte, and a truncated chain fails on the read rather than spinning.
bool skipSubBlocks(Reader& r, const uint8_t*& first, uint8_t& firstLen) {
  while (true) {
    uint8_t n;
    if (!r.byte(n)) return false;
    if (n == 0) return true;
    const uint8_t* p = r.take(n);
    if (!p) return false;
    if (firstLen == 0) {
      first = p;
      firstLen = n;
    }
  }
}

// LZW bit source: codes are packed LSB-first and run straight across sub-block
// boundaries, so a single code can straddle two sub-blocks.
struct BitReader {
  Reader& r;
  size_t left = 0;  // bytes remaining in the current sub-block
  uint32_t acc = 0;
  uint8_t bits = 0;
  bool ended = false;

  bool nextByte(uint8_t& v) {
    if (left == 0) {
      uint8_t n;
      if (ended || !r.byte(n) || n == 0) {
        ended = true;
        return false;
      }
      left = n;
    }
    if (!r.byte(v)) {
      ended = true;
      return false;
    }
    left--;
    return true;
  }

  // Returns the next `size`-bit code, or -1 when the data runs out.
  int code(uint8_t size) {
    while (bits < size) {
      uint8_t v;
      if (!nextByte(v)) return -1;
      acc |= (uint32_t)v << bits;
      bits = (uint8_t)(bits + 8);
    }
    const int out = (int)(acc & ((1u << size) - 1));
    acc >>= size;
    bits = (uint8_t)(bits - size);
    return out;
  }
};

// Sequential pixel sink that also performs de-interlacing, so the LZW loop
// never computes an output address itself. Writes stop at the last pixel:
// trailing data in an over-long stream is dropped, never written past the end.
struct PixelWriter {
  uint8_t* out;
  uint32_t w;
  uint32_t h;
  bool interlace;
  uint32_t x = 0;
  uint32_t y = 0;
  uint8_t pass = 0;
  bool done = false;

  static constexpr uint32_t PASS_START[4] = {0, 4, 2, 1};
  static constexpr uint32_t PASS_STEP[4] = {8, 8, 4, 2};

  void put(uint8_t v) {
    if (done) return;
    out[(size_t)y * w + x] = v;
    if (++x < w) return;
    x = 0;
    if (!interlace) {
      if (++y >= h) done = true;
      return;
    }
    y += PASS_STEP[pass];
    while (y >= h) {
      if (++pass > 3) {
        done = true;
        return;
      }
      y = PASS_START[pass];
    }
  }
};

// Decodes the LZW stream at `r` into `writer`. Stops on EOI, on exhausted
// input, or once the frame is full.
bool inflateLzw(Reader& r, PixelWriter& writer) {
  uint8_t minCodeSize;
  if (!r.byte(minCodeSize)) {
    LOG_ERR("GIF", "truncated before LZW data");
    return false;
  }
  if (minCodeSize < 2 || minCodeSize > 8) {
    LOG_ERR("GIF", "bad LZW min code size %u", minCodeSize);
    return false;
  }

  // 20 KB of scratch (dictionary + output stack), far past the 256-byte stack
  // budget, so it is one heap block scoped to this call. Fixed size: the
  // dictionary can never exceed 4096 entries.
  struct Dict {
    uint16_t prefix[MAX_CODES];
    uint8_t suffix[MAX_CODES];
    uint8_t stack[MAX_CODES + 2];  // +2: KwKwK byte and the final literal
  };
  auto dict = makeUniqueNoThrow<Dict>();
  if (!dict) {
    LOG_ERR("GIF", "OOM: %u bytes LZW scratch", (unsigned)sizeof(Dict));
    return false;
  }

  const uint16_t clearCode = (uint16_t)(1u << minCodeSize);
  const uint16_t eoiCode = (uint16_t)(clearCode + 1);
  uint16_t next = (uint16_t)(clearCode + 2);
  uint8_t codeSize = (uint8_t)(minCodeSize + 1);
  int prev = -1;
  uint8_t firstByte = 0;
  BitReader bits{r};

  // Bounded: every iteration either consumes at least 3 bits of input (and the
  // input is finite) or returns.
  while (!writer.done) {
    const int code = bits.code(codeSize);
    if (code < 0) break;  // truncated stream: keep what was decoded
    if (code == clearCode) {
      next = (uint16_t)(clearCode + 2);
      codeSize = (uint8_t)(minCodeSize + 1);
      prev = -1;
      continue;
    }
    if (code == eoiCode) break;

    if (prev < 0) {  // first code after a clear must be a literal
      if (code >= clearCode) {
        LOG_ERR("GIF", "LZW: first code %d is not a literal", code);
        return false;
      }
      firstByte = (uint8_t)code;
      writer.put(firstByte);
      prev = code;
      continue;
    }
    if (code > next || (code == next && next >= MAX_CODES)) {
      LOG_ERR("GIF", "LZW: code %d out of range (next %u)", code, next);
      return false;
    }

    // Unwind the code's string onto the stack (reversed), then emit it.
    // prefix[k] < k always holds, so the chain strictly descends and cannot
    // loop; the size check is a second line of defence.
    size_t sp = 0;
    int c = code;
    if (c == next) {  // KwKwK: string(prev) + first byte of string(prev)
      dict->stack[sp++] = firstByte;
      c = prev;
    }
    while (c >= clearCode + 2) {
      if (sp >= MAX_CODES) {
        LOG_ERR("GIF", "LZW: dictionary chain too long");
        return false;
      }
      dict->stack[sp++] = dict->suffix[c];
      c = dict->prefix[c];
    }
    if (c >= clearCode) {
      LOG_ERR("GIF", "LZW: corrupt dictionary entry %d", c);
      return false;
    }
    firstByte = (uint8_t)c;
    dict->stack[sp++] = firstByte;
    while (sp > 0) writer.put(dict->stack[--sp]);

    if (next < MAX_CODES) {
      dict->prefix[next] = (uint16_t)prev;
      dict->suffix[next] = firstByte;
      next++;
      if (next == (1u << codeSize) && codeSize < 12) codeSize++;
    }
    prev = code;
  }
  return true;
}

}  // namespace

bool decodeFirstFrame(const uint8_t* data, size_t len, Frame& frame) {
  // 13 = signature (6) + logical screen descriptor (7).
  if (!data || len < 13) {
    LOG_ERR("GIF", "truncated: %u bytes", (unsigned)len);
    return false;
  }
  if (memcmp(data, "GIF87a", 6) != 0 && memcmp(data, "GIF89a", 6) != 0) {
    LOG_ERR("GIF", "bad signature");
    return false;
  }

  Reader r{data, len, 6};
  const uint8_t* lsd = r.take(7);  // guaranteed by the length check above
  const uint8_t* palette = nullptr;
  uint16_t paletteCount = 0;
  if (lsd[4] & 0x80) {
    paletteCount = (uint16_t)(2u << (lsd[4] & 0x07));
    palette = r.take((size_t)paletteCount * 3);
    if (!palette) {
      LOG_ERR("GIF", "truncated global colour table");
      return false;
    }
  }
  int16_t transparentIndex = -1;

  // Block loop. Bounded: every path consumes at least one byte and any read
  // past the end returns false.
  while (true) {
    uint8_t block;
    if (!r.byte(block)) {
      LOG_ERR("GIF", "truncated before any image");
      return false;
    }
    if (block == 0x3B) {  // trailer
      LOG_ERR("GIF", "no image block");
      return false;
    }
    if (block == 0x21) {  // extension
      uint8_t label;
      if (!r.byte(label)) {
        LOG_ERR("GIF", "truncated extension");
        return false;
      }
      const uint8_t* payload = nullptr;
      uint8_t payloadLen = 0;
      if (!skipSubBlocks(r, payload, payloadLen)) {
        LOG_ERR("GIF", "truncated extension 0x%02X", label);
        return false;
      }
      // Graphic Control Extension: applies to the image that follows it.
      if (label == 0xF9 && payloadLen >= 4) {
        transparentIndex = (payload[0] & 0x01) ? (int16_t)payload[3] : (int16_t)-1;
      }
      continue;
    }
    if (block != 0x2C) {
      LOG_ERR("GIF", "unknown block 0x%02X", block);
      return false;
    }

    // Image descriptor: left, top, width, height (2 bytes each) + packed flags.
    const uint8_t* desc = r.take(9);
    if (!desc) {
      LOG_ERR("GIF", "truncated image descriptor");
      return false;
    }
    const uint32_t fw = le16(desc + 4);
    const uint32_t fh = le16(desc + 6);
    const bool interlace = (desc[8] & 0x40) != 0;
    if (desc[8] & 0x80) {  // local colour table overrides the global one
      paletteCount = (uint16_t)(2u << (desc[8] & 0x07));
      palette = r.take((size_t)paletteCount * 3);
      if (!palette) {
        LOG_ERR("GIF", "truncated local colour table");
        return false;
      }
    }
    if (fw == 0 || fh == 0) {
      LOG_ERR("GIF", "empty frame %ux%u", fw, fh);
      return false;
    }
    if ((uint64_t)fw * fh > MAX_PIXELS) {
      LOG_ERR("GIF", "frame too large: %ux%u", fw, fh);
      return false;
    }
    if (!palette || paletteCount == 0) {
      LOG_ERR("GIF", "frame has no colour table");
      return false;
    }

    // std::vector's allocation abort()s on OOM with -fno-exceptions, so probe
    // the block nothrow first: a big frame is then skipped with an error
    // instead of taking the firmware down.
    const size_t pixels = (size_t)fw * fh;
    {
      auto probe = makeUniqueNoThrow<uint8_t[]>(pixels);
      if (!probe) {
        LOG_ERR("GIF", "OOM: %ux%u frame", fw, fh);
        return false;
      }
    }
    frame.indices.assign(pixels, 0);
    if (frame.indices.size() != pixels) {
      LOG_ERR("GIF", "OOM: %ux%u frame", fw, fh);
      return false;
    }

    PixelWriter writer{frame.indices.data(), fw, fh, interlace};
    if (!inflateLzw(r, writer)) return false;

    // A malformed file can reference indices past the colour table. Pad the
    // palette instead of clamping the pixels: the PNG stays valid and no pixel
    // silently changes colour.
    uint16_t maxIndex = 0;
    for (const uint8_t v : frame.indices) {
      if (v > maxIndex) maxIndex = v;
    }
    frame.palette.assign(palette, palette + (size_t)paletteCount * 3);
    frame.count = paletteCount;
    if (maxIndex >= frame.count) {
      frame.count = (uint16_t)(maxIndex + 1);  // <= 256
      frame.palette.resize((size_t)frame.count * 3, 0);
    }
    frame.width = fw;
    frame.height = fh;
    frame.transparentIndex = (transparentIndex < (int16_t)frame.count) ? transparentIndex : (int16_t)-1;
    return true;  // first image wins
  }
}

}  // namespace gif
