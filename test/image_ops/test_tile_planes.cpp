#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>

#include "ImageOps.h"

namespace {

void reference(const uint8_t* levels, uint8_t* planes) {
  memset(planes, 0, 16);
  for (int row = 0; row < 8; ++row) {
    for (int column = 0; column < 8; ++column) {
      if (levels[row * 8 + column] & 1) planes[column] |= 128 >> row;
      if (levels[row * 8 + column] & 2) planes[8 + column] |= 128 >> row;
    }
  }
}

void check(const uint8_t* levels, size_t inputOffset, size_t outputOffset, bool repeatRows = false) {
  alignas(16) uint8_t input[112], output[64], packed[8][32];
  uint8_t expected[16], actualLevels[64];
  const uint8_t* rows[8];
  memset(input, 0x69, sizeof(input));
  memset(output, 0xa5, sizeof(output));
  memset(packed, 0x69, sizeof(packed));
  memcpy(input + 16 + inputOffset, levels, 64);
  reference(levels, expected);
  imageops::levelTile8x8ToPlanes(input + 16 + inputOffset, output + 16 + outputOffset);
  assert(memcmp(output + 16 + outputOffset, expected, 16) == 0);
  for (size_t i = 0; i < sizeof(input); ++i) {
    assert(input[i] == (i >= 16 + inputOffset && i < 80 + inputOffset ? levels[i - 16 - inputOffset] : 0x69));
  }
  for (size_t i = 0; i < sizeof(output); ++i) {
    if (i < 16 + outputOffset || i >= 32 + outputOffset) assert(output[i] == 0xa5);
  }

  for (size_t r = 0; r < 8; ++r) {
    const size_t offset = (inputOffset + r * 3) & 15;
    auto* row = packed[r] + offset;
    row[0] = row[1] = 0;
    for (int c = 0; c < 8; ++c) row[c / 4] |= levels[r * 8 + c] << (6 - (c & 3) * 2);
    rows[r] = repeatRows && r != 0 ? rows[0] : row;
  }
  uint8_t snapshot[sizeof(packed)];
  memcpy(snapshot, packed, sizeof(packed));
  for (int r = 0; r < 8; ++r) {
    for (int c = 0; c < 8; ++c) actualLevels[r * 8 + c] = (rows[r][c / 4] >> (6 - (c & 3) * 2)) & 3;
  }
  reference(actualLevels, expected);
  memset(output, 0xa5, sizeof(output));
  imageops::packed2BitTile8x8ToPlanes(rows, output + 16 + outputOffset);
  assert(memcmp(output + 16 + outputOffset, expected, 16) == 0);
  assert(memcmp(snapshot, packed, sizeof(packed)) == 0);
  for (size_t i = 0; i < sizeof(output); ++i) {
    if (i < 16 + outputOffset || i >= 32 + outputOffset) assert(output[i] == 0xa5);
  }
}

}  // namespace

void testTilePlanes() {
  uint8_t levels[64];
  uint32_t random = 0x693ac5d1;
  for (unsigned test = 0; test < 65536 + 128 + 1024; ++test) {
    for (unsigned i = 0; i < 64; ++i) {
      random ^= random << 13;
      random ^= random >> 17;
      random ^= random << 5;
      levels[i] = test < 65536         ? ((test >> ((i & 7) * 2)) ^ (i / 8)) & 3
                  : test < 65536 + 128 ? ((test - 65536) / 2 == i ? 1 << ((test - 65536) & 1) : 0)
                                       : random & 3;
    }
    check(levels, 0, 0);
  }
  for (size_t in = 0; in < 16; ++in) {
    for (size_t out = 0; out < 16; ++out) {
      check(levels, in, out);
      check(levels, in, out, true);
      // ASan redzones end immediately after every alignment's exact input/output.
      std::unique_ptr<uint8_t[]> input(new uint8_t[64 + in]);
      std::unique_ptr<uint8_t[]> output(new uint8_t[16 + out]);
      std::unique_ptr<uint8_t[]> packed[8];
      const uint8_t* rows[8];
      for (size_t r = 0; r < 8; ++r) {
        const size_t offset = (in + r) & 15;
        packed[r].reset(new uint8_t[2 + offset]);
        rows[r] = packed[r].get() + offset;
        packed[r][offset] = static_cast<uint8_t>(r * 37);
        packed[r][offset + 1] = static_cast<uint8_t>(r * 73);
      }
      memcpy(input.get() + in, levels, 64);
      imageops::levelTile8x8ToPlanes(input.get() + in, output.get() + out);
      uint8_t expected[16];
      reference(levels, expected);
      assert(memcmp(output.get() + out, expected, 16) == 0);
      for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) input[r * 8 + c] = (rows[r][c / 4] >> (6 - (c & 3) * 2)) & 3;
      }
      reference(input.get(), expected);
      imageops::packed2BitTile8x8ToPlanes(rows, output.get() + out);
      assert(memcmp(output.get() + out, expected, 16) == 0);
    }
  }
  for (int rows = 0; rows <= 8; ++rows) {
    for (int columns = 0; columns <= 8; ++columns) {
      memset(levels, 3, sizeof(levels));
      for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < columns; ++c) levels[r * 8 + c] = (r + c) & 3;
      }
      check(levels, 1, 3);
    }
  }
  puts(
      "PASS: ImageOps tile planes, patterns/basis/random, all alignments, independent/aliased rows, exact bounds, "
      "padding");
}
