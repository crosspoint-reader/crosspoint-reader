#include <cassert>
#include <cstddef>
#include <cstdint>

namespace IndexedModel {
size_t pixels[3] = {};
}

namespace {
void unpack(const uint8_t* input, uint8_t* output, size_t blocks, const uint8_t* mask, unsigned bits) {
  assert(blocks > 0);
  assert(
      ((reinterpret_cast<uintptr_t>(input) | reinterpret_cast<uintptr_t>(output) | reinterpret_cast<uintptr_t>(mask)) &
       15) == 0);
  const unsigned valueMask = (1u << bits) - 1;
  for (unsigned i = 0; i < 16; ++i) assert(mask[i] == valueMask);
  IndexedModel::pixels[bits == 1 ? 0 : bits == 2 ? 1 : 2] += blocks * 16 * (8 / bits);
  for (size_t i = 0; i < blocks * 16; ++i) {
    for (unsigned lane = 0; lane < 8 / bits; ++lane)
      *output++ = static_cast<uint8_t>((input[i] >> (8 - bits - lane * bits)) & valueMask);
  }
}
}  // namespace

extern "C" void imageOpsUnpackIndex1S3(const uint8_t* p, uint8_t* q, size_t n, const uint8_t* m) {
  unpack(p, q, n, m, 1);
}
extern "C" void imageOpsUnpackIndex2S3(const uint8_t* p, uint8_t* q, size_t n, const uint8_t* m) {
  unpack(p, q, n, m, 2);
}
extern "C" void imageOpsUnpackIndex4S3(const uint8_t* p, uint8_t* q, size_t n, const uint8_t* m) {
  unpack(p, q, n, m, 4);
}
