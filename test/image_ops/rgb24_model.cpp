#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Rgb24Model {
size_t pixels = 0;
size_t bgrPixels = 0;
}  // namespace Rgb24Model

// Model the packed word loads/register gather and signed weighted correction.
extern "C" void imageOpsRgb24GrayS3Blocks(const uint8_t* input, uint8_t* output, size_t blocks,
                                          const int16_t* weights) {
  assert(blocks > 0);
  assert(((reinterpret_cast<uintptr_t>(input) | reinterpret_cast<uintptr_t>(output)) & 3) == 0);
  assert((reinterpret_cast<uintptr_t>(weights) & 15) == 0);
  const bool bgr = weights[0] == 29;
  if (bgr)
    Rgb24Model::bgrPixels += blocks * 8;
  else
    Rgb24Model::pixels += blocks * 8;
  for (int lane = 0; lane < 8; ++lane) {
    assert(weights[lane] == (bgr ? 29 : 77) && weights[8 + lane] == -106 && weights[16 + lane] == (bgr ? 77 : 29) &&
           weights[24 + lane] == 1);
  }
  for (size_t group = 0; group < blocks * 2; ++group) {
    uint32_t words[3];
    memcpy(words, input + group * 12, sizeof(words));
    const uint32_t lanes[] = {words[0], (words[0] >> 24) | (words[1] << 8), (words[1] >> 16) | (words[2] << 16),
                              words[2] >> 8};
    for (int i = 0; i < 4; ++i) {
      const int first = lanes[i] & 255, g = (lanes[i] >> 8) & 255, last = (lanes[i] >> 16) & 255;
      const int correction = weights[0] * first + weights[8] * g + weights[16] * last;
      const int shifted = correction < 0 ? -((-correction + 255) / 256) : correction / 256;
      output[group * 4 + i] = static_cast<uint8_t>(g + shifted);
    }
  }
}
