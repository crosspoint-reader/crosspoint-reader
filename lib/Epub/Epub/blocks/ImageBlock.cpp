#include "ImageBlock.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

#include "../../../../src/CrossPointSettings.h"
#include "../../../../src/fontIds.h"
#include "../converters/DirectPixelWriter.h"
#include "../converters/ImageDecoderFactory.h"
#include "../converters/JpegToFramebufferConverter.h"
#include "../converters/PngToFramebufferConverter.h"

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - 2 bits per pixel, packed (4 pixels per byte), row-major order

ImageBlock::ImageBlock(const std::string& imagePath, int16_t width, int16_t height)
    : imagePath(imagePath), width(width), height(height) {}

bool ImageBlock::imageExists() const { return Storage.exists(imagePath.c_str()); }

namespace {

std::string getCachePath(const std::string& imagePath, ImageDitherMode ditherMode) {
  // Replace extension with .pxc (pixel cache)
  size_t dotPos = imagePath.rfind('.');
  if (dotPos != std::string::npos) {
    return imagePath.substr(0, dotPos) + getImageDitherCacheSuffix(ditherMode) + ".pxc";
  }
  return imagePath + getImageDitherCacheSuffix(ditherMode) + ".pxc";
}

bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                     int expectedHeight) {
  FsFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  if (cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) {
    cacheFile.close();
    return false;
  }

  // Verify dimensions are close (allow 1 pixel tolerance for rounding differences)
  int widthDiff = abs(cachedWidth - expectedWidth);
  int heightDiff = abs(cachedHeight - expectedHeight);
  if (widthDiff > 1 || heightDiff > 1) {
    LOG_ERR("IMG", "Cache dimension mismatch: %dx%d vs %dx%d", cachedWidth, cachedHeight, expectedWidth,
            expectedHeight);
    cacheFile.close();
    return false;
  }

  // Use cached dimensions for rendering (they're the actual decoded size)
  expectedWidth = cachedWidth;
  expectedHeight = cachedHeight;

  LOG_DBG("IMG", "Loading from cache: %s (%dx%d)", cachePath.c_str(), cachedWidth, cachedHeight);

  // Read and render row by row to minimize memory usage
  const int bytesPerRow = (cachedWidth + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
  uint8_t* rowBuffer = (uint8_t*)malloc(bytesPerRow);
  if (!rowBuffer) {
    LOG_ERR("IMG", "Failed to allocate row buffer");
    cacheFile.close();
    return false;
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  for (int row = 0; row < cachedHeight; row++) {
    if (cacheFile.read(rowBuffer, bytesPerRow) != bytesPerRow) {
      LOG_ERR("IMG", "Cache read error at row %d", row);
      free(rowBuffer);
      cacheFile.close();
      return false;
    }

    const int destY = y + row;
    pw.beginRow(destY);
    for (int col = 0; col < cachedWidth; col++) {
      const int byteIdx = col >> 2;            // col / 4
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      uint8_t pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;

      pw.writePixel(x + col, pixelValue);
    }
  }

  free(rowBuffer);
  cacheFile.close();
  LOG_DBG("IMG", "Cache render complete");
  return true;
}

}  // namespace

bool ImageBlock::isLargeImage() const {
  if (largeImageCached != 0) return largeImageCached == 1;
  ImageDimensions dims{0, 0};
  const bool ok = FsHelpers::hasJpgExtension(imagePath)
                      ? JpegToFramebufferConverter::getDimensionsStatic(imagePath, dims)
                      : PngToFramebufferConverter::getDimensionsStatic(imagePath, dims);
  if (ok && dims.width > 0 && dims.height > 0) {
    largeImageCached = (int32_t(dims.width) * dims.height > LARGE_IMAGE_PIXEL_THRESHOLD) ? 1 : -1;
  } else {
    largeImageCached = -1;  // unreadable header → assume not large, render normally
  }
  return largeImageCached == 1;
}

bool ImageBlock::wouldShowPlaceholder(bool forceLoad) const {
  if (forceLoad) return false;
  if (!isLargeImage()) return false;
  // If the pixel cache already exists the render is instant — no placeholder needed
  const ImageDitherMode ditherMode = imageDitherModeFromSetting(SETTINGS.imageDithering);
  const std::string pxcPath = [&] {
    size_t dot = imagePath.rfind('.');
    return (dot != std::string::npos ? imagePath.substr(0, dot) : imagePath) + getImageDitherCacheSuffix(ditherMode) +
           ".pxc";
  }();
  return !Storage.exists(pxcPath.c_str());
}

void ImageBlock::renderPlaceholder(GfxRenderer& renderer, const int x, const int y) const {
  constexpr int BORDER = 1;
  constexpr int PADDING = 6;

  renderer.drawRect(x, y, width, height, BORDER, true);

  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int totalTextH = lineH * 2;

  if (lineH > 0 && width > PADDING * 2 && height > totalTextH + PADDING * 2) {
    const int textX = x + PADDING;
    const int textY = y + (height - totalTextH) / 2;
    renderer.drawText(UI_10_FONT_ID, textX, textY, "Image");
    renderer.drawText(UI_10_FONT_ID, textX, textY + lineH, "Press OK to load");
  }
}

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y, const bool forceLoad) {
  LOG_DBG("IMG", "Rendering image at %d,%d: %s (%dx%d)", x, y, imagePath.c_str(), width, height);

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  // Bounds check render position using logical screen dimensions
  if (x < 0 || y < 0 || x + width > screenWidth || y + height > screenHeight) {
    LOG_ERR("IMG", "Render bounds rejected: (%d,%d) size (%dx%d) screen (%dx%d)", x, y, width, height, screenWidth,
            screenHeight);
    return;
  }

  // Try to render from pixel cache first (always, regardless of forceLoad)
  const ImageDitherMode ditherMode = imageDitherModeFromSetting(SETTINGS.imageDithering);
  std::string cachePath = getCachePath(imagePath, ditherMode);
  if (renderFromCache(renderer, cachePath, x, y, width, height)) {
    return;
  }

  // No pixel cache — check if this is a large image that should show a placeholder
  if (wouldShowPlaceholder(forceLoad)) {
    LOG_DBG("IMG", "Large image placeholder at %d,%d (%dx%d): %s", x, y, width, height, imagePath.c_str());
    renderPlaceholder(renderer, x, y);
    return;
  }

  // Proceed with full decode
  FsFile file;
  if (!Storage.openFileForRead("IMG", imagePath, file)) {
    LOG_ERR("IMG", "Image file not found: %s", imagePath.c_str());
    return;
  }
  size_t fileSize = file.size();
  file.close();

  if (fileSize == 0) {
    LOG_ERR("IMG", "Image file is empty: %s", imagePath.c_str());
    return;
  }

  LOG_DBG("IMG", "Decoding and caching: %s", imagePath.c_str());

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = true;
  config.useDithering = true;
  config.ditherMode = ditherMode;
  config.performanceMode = false;
  config.useExactDimensions = true;
  config.cachePath = cachePath;

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    LOG_ERR("IMG", "No decoder found for image: %s", imagePath.c_str());
    return;
  }

  LOG_DBG("IMG", "Using %s decoder", decoder->getFormatName());

  bool success = decoder->decodeToFramebuffer(imagePath, renderer, config);
  if (!success) {
    LOG_ERR("IMG", "Failed to decode image: %s", imagePath.c_str());
  }
}

bool ImageBlock::serialize(FsFile& file) {
  serialization::writeString(file, imagePath);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  return true;
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(FsFile& file) {
  std::string path;
  serialization::readString(file, path);
  int16_t w, h;
  serialization::readPod(file, w);
  serialization::readPod(file, h);
  return std::unique_ptr<ImageBlock>(new ImageBlock(path, w, h));
}
