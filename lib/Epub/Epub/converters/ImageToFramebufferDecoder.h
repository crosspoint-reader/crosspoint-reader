#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

class GfxRenderer;

struct ImageDimensions {
  int16_t width;
  int16_t height;
};

enum class ImageDitherMode : uint8_t {
  Bayer = 0,
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
  Atkinson = 1,
  DiffusedBayer = 2,
#endif
  COUNT,
};

inline ImageDitherMode imageDitherModeFromSetting(uint8_t value) {
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
  switch (static_cast<ImageDitherMode>(value)) {
    case ImageDitherMode::Bayer:
    case ImageDitherMode::Atkinson:
    case ImageDitherMode::DiffusedBayer:
      return static_cast<ImageDitherMode>(value);
    case ImageDitherMode::COUNT:
    default:
      return ImageDitherMode::Bayer;
  }
#else
  (void)value;
  return ImageDitherMode::Bayer;
#endif
}

inline const char* getImageDitherCacheSuffix(ImageDitherMode mode) {
  switch (mode) {
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
    case ImageDitherMode::Atkinson:
      return ".atkinson";
    case ImageDitherMode::DiffusedBayer:
      return ".diffused-bayer";
#endif
    case ImageDitherMode::Bayer:
    case ImageDitherMode::COUNT:
    default:
      return ".bayer";
  }
}

struct RenderConfig {
  int x, y;
  int maxWidth, maxHeight;
  bool useGrayscale = true;
  bool useDithering = true;
  ImageDitherMode ditherMode = ImageDitherMode::Bayer;
  bool performanceMode = false;
  bool useExactDimensions = false;  // If true, use maxWidth/maxHeight as exact output size (no recalculation)
  // If true, the decoder uses a 1-bit Atkinson dither and emits only the values 0/3 — suitable for
  // pure black-and-white display (no grayscale planes). The 4-level dither path produces values 1/2
  // which the BW DirectPixelWriter collapses to black, making mid-grays render very dark.
  bool monochromeOutput = false;
  std::string cachePath;  // If non-empty, decoder will write pixel cache to this path
};

class ImageToFramebufferDecoder {
 public:
  virtual ~ImageToFramebufferDecoder() = default;

  virtual bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) = 0;

  virtual bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const = 0;

  virtual const char* getFormatName() const = 0;

 protected:
  // Size validation helpers
  static constexpr int MAX_SOURCE_PIXELS = 3145728;  // 2048 * 1536

  bool validateImageDimensions(int width, int height, const std::string& format);
  void warnUnsupportedFeature(const std::string& feature, const std::string& imagePath);
};
