#include <cassert>
#include <climits>
#include <cstdio>
#include <cstring>
#include <memory>

#include "ImageOps.h"

namespace {

constexpr size_t CAPACITY = 864;
alignas(16) uint8_t input[CAPACITY];
alignas(16) uint8_t original[CAPACITY];
alignas(16) uint8_t output[CAPACITY];
alignas(16) uint8_t expected[CAPACITY];
alignas(16) uint8_t inPlace[CAPACITY];

uint8_t reference(uint8_t gray, unsigned x, unsigned y) {
  static constexpr uint8_t MATRIX[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
  int adjusted = gray + (static_cast<int>(MATRIX[(y % 4) * 4 + x % 4]) - 8) * 5;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;
  return adjusted < 64 ? 0 : adjusted < 128 ? 1 : adjusted < 192 ? 2 : 3;
}

void check(size_t count, size_t inputOffset, size_t outputOffset, int x, int y) {
  assert(count + inputOffset <= CAPACITY && count + outputOffset <= CAPACITY);
  memcpy(original, input, CAPACITY);
  memset(output, 0xa5, CAPACITY);
  memset(expected, 0xa5, CAPACITY);
  for (size_t i = 0; i < count; ++i) {
    expected[outputOffset + i] = reference(input[inputOffset + i], static_cast<unsigned>(x) + i, y);
  }
  imageops::ditherBayer4LevelSpan(input + inputOffset, output + outputOffset, count, x, y);
  assert(memcmp(output, expected, CAPACITY) == 0);
  assert(memcmp(input, original, CAPACITY) == 0);

  memcpy(inPlace, input, CAPACITY);
  imageops::ditherBayer4LevelSpan(inPlace + inputOffset, inPlace + inputOffset, count, x, y);
  for (size_t i = 0; i < CAPACITY; ++i) {
    const uint8_t wanted = i >= inputOffset && i - inputOffset < count
                               ? reference(input[i], static_cast<unsigned>(x) + i - inputOffset, y)
                               : input[i];
    assert(inPlace[i] == wanted);
  }
}

}  // namespace

void testTilePlanes();
void testRgb24();
void testIndexed();

int main() {
  testTilePlanes();
  testRgb24();
  testIndexed();
  imageops::ditherBayer4LevelSpan(nullptr, nullptr, 0, INT_MIN, INT_MAX);
  imageops::ditherBayer4LevelSpan(input, nullptr, 0, 0, 0);
  imageops::ditherBayer4LevelSpan(nullptr, output, 0, 0, 0);
  for (int gray = 0; gray < 256; ++gray) {
    memset(input, gray, CAPACITY);
    for (int y = 0; y < 4; ++y) {
      for (int x = 0; x < 4; ++x) check(32, 16, 16, x, y);
    }
  }
  uint32_t random = 0x12345678;
  for (auto& value : input) {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    value = random >> 24;
  }
  constexpr size_t LARGE[] = {127, 128, 129, 480, 799, 800};
  for (size_t test = 0; test < 82 + sizeof(LARGE) / sizeof(LARGE[0]); ++test) {
    const size_t count = test < 82 ? test : LARGE[test - 82];
    for (size_t in = 16; in < 32; ++in) {
      for (size_t out = 16; out < 32; ++out) check(count, in, out, -static_cast<int>(in), -static_cast<int>(out));
    }
    // Redzones immediately follow every alignment's valid span, including staging.
    for (size_t offset = 0; count && offset < 16; ++offset) {
      std::unique_ptr<uint8_t[]> source(new uint8_t[count + offset]);
      std::unique_ptr<uint8_t[]> destination(new uint8_t[count + offset]);
      memset(source.get(), 0x5a, count + offset);
      memset(destination.get(), 0xa5, count + offset);
      memcpy(source.get() + offset, input, count);
      imageops::ditherBayer4LevelSpan(source.get() + offset, destination.get() + offset, count, INT_MAX, INT_MIN);
      for (size_t i = 0; i < count; ++i) {
        assert(destination[offset + i] == reference(input[i], unsigned(INT_MAX) + i, INT_MIN));
        assert(source[offset + i] == input[i]);
      }
      for (size_t i = 0; i < offset; ++i) assert(source[i] == 0x5a && destination[i] == 0xa5);
      imageops::ditherBayer4LevelSpan(source.get() + offset, source.get() + offset, count, INT_MIN, INT_MAX);
      for (size_t i = 0; i < count; ++i) {
        assert(source[offset + i] == reference(input[i], unsigned(INT_MIN) + i, INT_MAX));
      }
      for (size_t i = 0; i < offset; ++i) assert(source[i] == 0x5a);
    }
  }
  check(800, 16, 16, INT_MAX, INT_MIN);
  check(799, 17, 18, INT_MIN, INT_MAX);
  puts(
      "PASS: ImageOps Bayer luminances/phases/lanes, alignments/tails, guards, exact-size reads, in-place and extreme "
      "coordinates");
}
