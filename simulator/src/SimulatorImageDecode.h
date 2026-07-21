#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace simulator_image {

struct DecodedImage {
  int width{0};
  int height{0};
  int channels{0};
  bool hasAlpha{false};
  std::vector<uint8_t> pixels;
};

bool decodeImageBytes(const uint8_t *data, size_t size, int requestedChannels,
                      DecodedImage &out);
const char *lastDecodeError();

} // namespace simulator_image
