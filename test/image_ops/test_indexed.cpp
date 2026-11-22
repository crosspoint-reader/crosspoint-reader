#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>

#include "ImageOps.h"

namespace {
using imageops::IndexDepth;
constexpr IndexDepth DEPTHS[] = {IndexDepth::Bits1, IndexDepth::Bits2, IndexDepth::Bits4, IndexDepth::Bits8};
alignas(16) uint8_t input[1088], original[1088], output[864], expected[864];
uint8_t palette[1024], paletteCopy[1024], gray[272], alpha[272];

uint8_t indexAt(const uint8_t* data, unsigned bits, size_t x) {
  return (data[x * bits / 8] >> (8 - bits - (x * bits % 8))) & ((1u << bits) - 1);
}

void check(IndexDepth depth, size_t first, size_t count, size_t in, size_t out) {
  const unsigned bits = static_cast<unsigned>(depth);
  memcpy(original, input, sizeof(input));
  memset(output, 0xa5, sizeof(output));
  memset(expected, 0xa5, sizeof(expected));
  for (size_t x = 0; x < count; ++x) expected[out + x] = indexAt(input + in, bits, first + x);
  imageops::unpackIndexSpan(input + in, output + out, count, depth, first);
  assert(memcmp(output, expected, sizeof(output)) == 0);
  assert(memcmp(input, original, sizeof(input)) == 0);
}

void paletteCheck(IndexDepth depth, bool hasAlpha, bool retainAlpha, const uint8_t* colors) {
  const unsigned entries = 1u << static_cast<unsigned>(depth);
  memset(gray, 0xa5, sizeof(gray));
  memset(alpha, 0xa5, sizeof(alpha));
  imageops::prepareIndexedPalette(colors, depth, hasAlpha, retainAlpha, gray + 7, alpha + 9);
  for (unsigned index = 0; index < entries; ++index) {
    const unsigned value =
        colors ? (77u * colors[index * 3] + 150u * colors[index * 3 + 1] + 29u * colors[index * 3 + 2]) / 256
               : index * 255u / (entries - 1);
    const unsigned a = colors && hasAlpha ? colors[768 + index] : 255;
    assert(gray[7 + index] == (retainAlpha ? value : (value * a + 255u * (255u - a)) / 255u));
    assert(alpha[9 + index] == a);
  }
  for (unsigned i = 0; i < sizeof(gray); ++i)
    if (i < 7 || i >= 7 + entries) assert(gray[i] == 0xa5);
  for (unsigned i = 0; i < sizeof(alpha); ++i)
    if (i < 9 || i >= 9 + entries) assert(alpha[i] == 0xa5);
}
}  // namespace

void testIndexed() {
  for (auto depth : DEPTHS) {
    imageops::unpackIndexSpan(nullptr, nullptr, 0, depth, static_cast<size_t>(-1));
    for (unsigned value = 0; value < 256; ++value) {
      memset(input, value, sizeof(input));
      for (size_t first = 0; first < 8; ++first) check(depth, first, 256, 16 + (value & 15), 16 + first);
    }
  }
  uint32_t random = 0x678abcde;
  for (auto& value : input) {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    value = random >> 24;
  }
  constexpr size_t LARGE[] = {63, 64, 127, 128, 129, 193, 479, 480, 799, 800};
  for (auto depth : DEPTHS) {
    const unsigned bits = static_cast<unsigned>(depth);
    for (size_t test = 0; test < 34 + sizeof(LARGE) / sizeof(LARGE[0]); ++test) {
      const size_t count = test < 34 ? test : LARGE[test - 34];
      for (size_t first = 0; first < 16; ++first) {
        for (size_t in = 16; in < 32; ++in)
          for (size_t out = 16; out < 32; ++out) check(depth, first, count, in, out);
        if (!count) continue;
        const size_t bytes = ((first + count) * bits + 7) / 8;
        for (size_t offset = 0; offset < 16; ++offset) {
          std::unique_ptr<uint8_t[]> source(new uint8_t[bytes + offset]);
          std::unique_ptr<uint8_t[]> destination(new uint8_t[count + offset]);
          memset(source.get(), 0x5a, bytes + offset);
          memset(destination.get(), 0xa5, count + offset);
          memcpy(source.get() + offset, input, bytes);
          imageops::unpackIndexSpan(source.get() + offset, destination.get() + offset, count, depth, first);
          for (size_t x = 0; x < count; ++x) assert(destination[offset + x] == indexAt(input, bits, first + x));
          assert(memcmp(source.get() + offset, input, bytes) == 0);
          for (size_t i = 0; i < offset; ++i) assert(source[i] == 0x5a && destination[i] == 0xa5);
        }
      }
    }
  }
  for (unsigned i = 0; i < sizeof(palette); ++i) palette[i] = static_cast<uint8_t>(i * 73 + (i >> 3));
  memcpy(paletteCopy, palette, sizeof(palette));
  for (auto depth : DEPTHS) {
    for (bool hasAlpha : {false, true}) {
      for (bool retainAlpha : {false, true}) {
        paletteCheck(depth, hasAlpha, retainAlpha, palette);
        paletteCheck(depth, hasAlpha, retainAlpha, nullptr);
        const size_t bytes =
            hasAlpha ? 768 + (1u << static_cast<unsigned>(depth)) : 3 * (1u << static_cast<unsigned>(depth));
        std::unique_ptr<uint8_t[]> exact(new uint8_t[bytes]);
        memcpy(exact.get(), palette, bytes);
        paletteCheck(depth, hasAlpha, retainAlpha, exact.get());
      }
    }
  }
  assert(memcmp(palette, paletteCopy, sizeof(palette)) == 0);
  for (unsigned value = 0; value < 256; ++value) {
    palette[0] = palette[1] = palette[2] = value;
    for (unsigned a = 0; a < 256; ++a) {
      palette[768] = a;
      paletteCheck(IndexDepth::Bits1, true, false, palette);
      paletteCheck(IndexDepth::Bits1, true, true, palette);
    }
  }
  puts(
      "PASS: ImageOps indexed depths/bytes/lanes/crops/alignments/tails/redzones; palette/alpha/65536 blend pairs/null "
      "fallback");
}
