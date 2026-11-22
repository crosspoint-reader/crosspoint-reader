#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

void aligned(const void* pointer) { assert((reinterpret_cast<uintptr_t>(pointer) & 15) == 0); }

void transpose(uint8_t* tile) {
  uint8_t registers[4][16];
  memcpy(registers, tile, 64);
  const auto zip = [&](int first, int second, int size) {
    uint8_t joined[32];
    for (int i = 0; i < 16 / size; ++i) {
      memcpy(joined + i * size * 2, registers[first] + i * size, size);
      memcpy(joined + (i * 2 + 1) * size, registers[second] + i * size, size);
    }
    memcpy(registers[first], joined, 16);
    memcpy(registers[second], joined + 16, 16);
  };
  zip(0, 1, 1);
  zip(2, 3, 1);
  zip(0, 2, 2);
  zip(1, 3, 2);
  zip(0, 1, 1);
  zip(2, 3, 1);
  memcpy(tile, registers, 64);
}

}  // namespace

// Linked host models of the retained ZIP/shift/mask kernels, not target execution.
extern "C" void imageOpsLevelTilePlanesS3(const uint8_t* input, uint8_t* planes, const uint32_t* masks) {
  aligned(input);
  aligned(planes);
  aligned(masks);
  uint8_t registers[64];
  for (int row = 0; row < 8; ++row) memcpy(registers + row * 8, input + (7 - row) * 8, 8);
  transpose(registers);
  for (int word = 0; word < 16; ++word) {
    uint32_t x;
    memcpy(&x, registers + word * 4, 4);
    x = (x | (x >> 6)) & masks[0];
    x = (x | (x >> 12)) & masks[4];
    memcpy(registers + word * 4, &x, 4);
  }
  const auto unzip = [&](int first, int second) {
    uint8_t joined[32];
    for (int i = 0; i < 8; ++i) {
      joined[i] = registers[first * 16 + i * 2];
      joined[8 + i] = registers[second * 16 + i * 2];
      joined[16 + i] = registers[first * 16 + i * 2 + 1];
      joined[24 + i] = registers[second * 16 + i * 2 + 1];
    }
    memcpy(registers + first * 16, joined, 16);
    memcpy(registers + second * 16, joined + 16, 16);
  };
  unzip(0, 1);
  unzip(2, 3);
  unzip(0, 2);
  uint8_t fragments[32];
  for (int plane = 0; plane < 2; ++plane) {
    for (int word = 0; word < 4; ++word) {
      uint32_t x;
      memcpy(&x, registers + word * 4, 4);
      x = static_cast<uint32_t>(static_cast<int32_t>(x) >> plane) & masks[8];
      x = (x | (x >> 1)) & masks[12];
      x = (x | (x >> 2)) & masks[16];
      x = (x | (x >> 4)) & masks[20];
      memcpy(fragments + plane * 16 + word * 4, &x, 4);
    }
  }
  for (int i = 0; i < 16; ++i) planes[i] = fragments[i * 2];
}

extern "C" void imageOpsPackedTilePlanesS3(const uint8_t* input, uint8_t* planes, const uint32_t* masks) {
  aligned(input);
  aligned(planes);
  aligned(masks);
  uint32_t fragments[2][4];
  for (int lane = 0; lane < 4; ++lane) {
    uint32_t x;
    memcpy(&x, input + lane * 4, 4);
    uint32_t t = ((x >> 10) ^ x) & masks[0];
    x ^= t ^ (t << 10);
    t = ((x >> 20) ^ x) & masks[4];
    x ^= t ^ (t << 20);
    for (int plane = 0; plane < 2; ++plane) {
      uint32_t bits = (static_cast<uint32_t>(static_cast<int32_t>(x) >> plane)) & masks[8];
      bits = (bits | (bits >> 1)) & masks[12];
      bits = (bits | (bits >> 2)) & masks[16];
      fragments[plane][lane] = bits;
    }
  }
  for (int plane = 0; plane < 2; ++plane) {
    for (int half = 0; half < 2; ++half) {
      const uint32_t word = (fragments[plane][2 * half] << 4) | fragments[plane][2 * half + 1];
      memcpy(planes + plane * 8 + half * 4, &word, 4);
    }
  }
}
