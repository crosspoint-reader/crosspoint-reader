#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>

#include "ImageOps.h"

#if USE_SIMD_S3_PIE
namespace Rgb24Model {
extern size_t pixels, bgrPixels;
}
#endif

namespace {
alignas(16) uint8_t input[2464], original[2464];
alignas(16) uint8_t output[864], expected[864];

using Convert = void (*)(const uint8_t*, uint8_t*, size_t);

uint8_t reference(const uint8_t* input, bool bgr) {
  return static_cast<uint8_t>((77u * input[bgr ? 2 : 0] + 150u * input[1] + 29u * input[bgr ? 0 : 2]) / 256);
}

void check(size_t count, size_t in, size_t out, Convert convert, bool bgr) {
  memcpy(original, input, sizeof(input));
  memset(output, 0xa5, sizeof(output));
  memset(expected, 0xa5, sizeof(expected));
  for (size_t i = 0; i < count; ++i) expected[out + i] = reference(input + in + i * 3, bgr);
  convert(input + in, output + out, count);
  assert(memcmp(output, expected, sizeof(output)) == 0);
  assert(memcmp(input, original, sizeof(input)) == 0);
}

void testOrder(bool bgr) {
  const Convert convert = bgr ? imageops::bgr24ToGraySpan : imageops::rgb24ToGraySpan;
#if USE_SIMD_S3_PIE
  const size_t beforeRgb = Rgb24Model::pixels, beforeBgr = Rgb24Model::bgrPixels;
#endif
  convert(nullptr, nullptr, 0);
  for (uint32_t base = 0; base < 0x1000000; base += 256) {
    for (unsigned i = 0; i < 256; ++i) {
      input[3 * i] = (base + i) >> 16;
      input[3 * i + 1] = (base + i) >> 8;
      input[3 * i + 2] = base + i;
    }
    convert(input, output, 256);
    for (size_t i = 0; i < 256; ++i) assert(output[i] == reference(input + i * 3, bgr));
  }
  uint32_t random = 0x7869ab12;
  for (auto& value : input) {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    value = random >> 24;
  }
  constexpr size_t LARGE[] = {63, 64, 65, 127, 480, 799, 800};
  for (size_t test = 0; test < 34 + sizeof(LARGE) / sizeof(LARGE[0]); ++test) {
    const size_t count = test < 34 ? test : LARGE[test - 34];
    for (size_t in = 16; in < 32; ++in) {
      for (size_t out = 16; out < 32; ++out) check(count, in, out, convert, bgr);
    }
    if (count == 0) continue;
    for (size_t in = 0; in < 16; ++in) {
      for (size_t out = 0; out < 16; ++out) {
        std::unique_ptr<uint8_t[]> source(new uint8_t[count * 3 + in]);
        std::unique_ptr<uint8_t[]> destination(new uint8_t[count + out]);
        memset(source.get(), 0x5a, count * 3 + in);
        memset(destination.get(), 0xa5, count + out);
        memcpy(source.get() + in, input, count * 3);
        convert(source.get() + in, destination.get() + out, count);
        for (size_t i = 0; i < count; ++i) assert(destination[out + i] == reference(input + i * 3, bgr));
        assert(memcmp(source.get() + in, input, count * 3) == 0);
        for (size_t i = 0; i < in; ++i) assert(source[i] == 0x5a);
        for (size_t i = 0; i < out; ++i) assert(destination[i] == 0xa5);
      }
    }
  }
#if USE_SIMD_S3_PIE
  assert(bgr ? Rgb24Model::bgrPixels > beforeBgr : Rgb24Model::pixels > beforeRgb);
  assert(bgr ? Rgb24Model::pixels == beforeRgb : Rgb24Model::bgrPixels == beforeBgr);
#endif
  printf(
      "PASS: ImageOps %s all 16777216 colors, mixed lanes, alignments/tails, guards, exact-size reads and source "
      "preservation\n",
      bgr ? "BGR24" : "RGB24");
}
}  // namespace

void testRgb24() {
  testOrder(false);
  testOrder(true);
}
