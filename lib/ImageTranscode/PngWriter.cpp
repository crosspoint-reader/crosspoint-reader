#include "PngWriter.h"

#include <Logging.h>
#include <MinizConfig.h>

#include <cstring>

namespace png {
namespace {

void be32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back((uint8_t)(x >> 24));
  v.push_back((uint8_t)(x >> 16));
  v.push_back((uint8_t)(x >> 8));
  v.push_back((uint8_t)x);
}

void chunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* data, size_t len) {
  be32(out, (uint32_t)len);
  const size_t crcStart = out.size();
  out.insert(out.end(), type, type + 4);
  if (len) out.insert(out.end(), data, data + len);
  const uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, out.data() + crcStart, out.size() - crcStart);
  be32(out, crc);
}

constexpr size_t STORED_BLOCK_MAX = 65535;

}  // namespace

bool encode(const uint8_t* pixels, uint32_t width, uint32_t height, Color color, const Palette& palette,
            std::vector<uint8_t>& out) {
  if (!pixels || width == 0 || height == 0) return false;
  if (color == Color::Palette8 && (!palette.rgb || palette.count == 0 || palette.count > 256)) return false;

  const uint32_t bytesPerPixel = (color == Color::Rgb8) ? 3 : 1;
  const uint64_t rowBytes = (uint64_t)width * bytesPerPixel;
  // Raw zlib payload: one filter byte (0 = None) per scanline, then the row.
  const uint64_t rawLen = (rowBytes + 1) * height;
  if (rawLen > 32u * 1024 * 1024) {
    LOG_ERR("PNG", "image too large: %ux%u", width, height);
    return false;
  }

  out.clear();
  static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  out.insert(out.end(), SIG, SIG + 8);

  uint8_t ihdr[13];
  ihdr[0] = (uint8_t)(width >> 24);
  ihdr[1] = (uint8_t)(width >> 16);
  ihdr[2] = (uint8_t)(width >> 8);
  ihdr[3] = (uint8_t)width;
  ihdr[4] = (uint8_t)(height >> 24);
  ihdr[5] = (uint8_t)(height >> 16);
  ihdr[6] = (uint8_t)(height >> 8);
  ihdr[7] = (uint8_t)height;
  ihdr[8] = 8;  // bit depth
  ihdr[9] = (color == Color::Rgb8) ? 2 : (color == Color::Palette8 ? 3 : 0);
  ihdr[10] = 0;  // deflate
  ihdr[11] = 0;  // filter method 0
  ihdr[12] = 0;  // no interlace
  chunk(out, "IHDR", ihdr, sizeof(ihdr));

  if (color == Color::Palette8) {
    chunk(out, "PLTE", palette.rgb, (size_t)palette.count * 3);
    if (palette.transparentIndex >= 0 && palette.transparentIndex < palette.count) {
      // tRNS for an indexed image is one alpha byte per entry, up to and
      // including the transparent one.
      std::vector<uint8_t> trns((size_t)palette.transparentIndex + 1, 0xFF);
      trns[(size_t)palette.transparentIndex] = 0x00;
      chunk(out, "tRNS", trns.data(), trns.size());
    }
  }

  // IDAT: zlib wrapper (0x78 0x01 => deflate, 32K window, no dict; 0x7801 % 31 == 0)
  // around stored deflate blocks, then the adler32 of the raw bytes.
  std::vector<uint8_t> idat;
  idat.reserve((size_t)rawLen + (size_t)(rawLen / STORED_BLOCK_MAX + 1) * 5 + 6);
  idat.push_back(0x78);
  idat.push_back(0x01);

  uint32_t adler = (uint32_t)MZ_ADLER32_INIT;
  // Walk the raw stream (filter byte + row, per row) while chopping it into
  // stored blocks of at most 65535 bytes; block boundaries are independent of
  // row boundaries, so the two loops are interleaved through a small staging
  // buffer.
  std::vector<uint8_t> block;
  block.reserve(STORED_BLOCK_MAX);
  uint64_t emitted = 0;

  auto flushBlock = [&](bool final) {
    idat.push_back(final ? 0x01 : 0x00);
    const uint16_t len = (uint16_t)block.size();
    idat.push_back((uint8_t)(len & 0xFF));
    idat.push_back((uint8_t)(len >> 8));
    idat.push_back((uint8_t)(~len & 0xFF));
    idat.push_back((uint8_t)((~len >> 8) & 0xFF));
    idat.insert(idat.end(), block.begin(), block.end());
    adler = (uint32_t)mz_adler32(adler, block.data(), block.size());
    emitted += block.size();
    block.clear();
  };

  for (uint32_t y = 0; y < height; y++) {
    const uint8_t filterByte = 0;
    const uint8_t* row = pixels + (uint64_t)y * rowBytes;
    // filter byte
    if (block.size() == STORED_BLOCK_MAX) flushBlock(false);
    block.push_back(filterByte);
    // row bytes
    uint64_t left = rowBytes;
    const uint8_t* p = row;
    while (left > 0) {
      if (block.size() == STORED_BLOCK_MAX) flushBlock(false);
      const size_t room = STORED_BLOCK_MAX - block.size();
      const size_t take = (left < room) ? (size_t)left : room;
      block.insert(block.end(), p, p + take);
      p += take;
      left -= take;
    }
  }
  flushBlock(true);
  if (emitted != rawLen) {
    LOG_ERR("PNG", "internal: emitted %llu of %llu", (unsigned long long)emitted, (unsigned long long)rawLen);
    return false;
  }

  idat.push_back((uint8_t)(adler >> 24));
  idat.push_back((uint8_t)(adler >> 16));
  idat.push_back((uint8_t)(adler >> 8));
  idat.push_back((uint8_t)adler);

  chunk(out, "IDAT", idat.data(), idat.size());
  chunk(out, "IEND", nullptr, 0);
  return true;
}

}  // namespace png
