#include "SimulatorImageDecode.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace simulator_image {

bool decodeImageBytes(const uint8_t *data, size_t size, int requestedChannels,
                      DecodedImage &out) {
  out = DecodedImage{};
  if (!data || size == 0 || requestedChannels <= 0) {
    return false;
  }

  int width = 0;
  int height = 0;
  int sourceChannels = 0;
  stbi_uc *decoded =
      stbi_load_from_memory(data, static_cast<int>(size), &width, &height,
                            &sourceChannels, requestedChannels);
  if (!decoded || width <= 0 || height <= 0) {
    if (decoded) {
      stbi_image_free(decoded);
    }
    return false;
  }

  const size_t byteCount =
      static_cast<size_t>(width) * static_cast<size_t>(height) *
      static_cast<size_t>(requestedChannels);
  out.width = width;
  out.height = height;
  out.channels = requestedChannels;
  out.hasAlpha = sourceChannels == 2 || sourceChannels == 4;
  out.pixels.assign(decoded, decoded + byteCount);
  stbi_image_free(decoded);
  return true;
}

const char *lastDecodeError() { return stbi_failure_reason(); }

} // namespace simulator_image
