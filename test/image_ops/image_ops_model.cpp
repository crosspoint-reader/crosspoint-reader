#include <cassert>
#include <cstddef>
#include <cstdint>

// Model the signed saturation and word shift; this does not execute S3 instructions.
extern "C" void imageOpsBayerSpanS3Blocks(const uint8_t* input, uint8_t* output, size_t blocks, const int8_t* offsets,
                                          const uint8_t* constants) {
  assert(blocks != 0);
  assert(((reinterpret_cast<uintptr_t>(input) | reinterpret_cast<uintptr_t>(output) |
           reinterpret_cast<uintptr_t>(offsets) | reinterpret_cast<uintptr_t>(constants)) &
          15) == 0);
  for (size_t block = 0; block < blocks; ++block) {
    for (size_t wordIndex = 0; wordIndex < 4; ++wordIndex) {
      uint32_t word = 0;
      for (size_t lane = 0; lane < 4; ++lane) {
        const size_t i = wordIndex * 4 + lane;
        int value = static_cast<int>(input[i]) - constants[i] + offsets[i];
        if (value < -128) value = -128;
        if (value > 127) value = 127;
        word |= static_cast<uint32_t>(value + 128) << (lane * 8);
      }
      word >>= 6;
      for (size_t lane = 0; lane < 4; ++lane) {
        output[wordIndex * 4 + lane] = (word >> (lane * 8)) & constants[16 + lane];
      }
    }
    input += 16;
    output += 16;
  }
}
