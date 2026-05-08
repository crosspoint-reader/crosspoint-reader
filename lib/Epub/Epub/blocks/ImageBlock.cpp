#include "ImageBlock.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <JpegRender.h>
#include <Logging.h>
#include <Serialization.h>

#include "../converters/DirectPixelWriter.h"
#include "../converters/ImageDecoderFactory.h"

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - 2 bits per pixel, packed (4 pixels per byte), row-major order

ImageBlock::ImageBlock(const std::string& imagePath, int16_t width, int16_t height)
    : imagePath(imagePath), width(width), height(height) {}

bool ImageBlock::imageExists() const { return Storage.exists(imagePath.c_str()); }

namespace {

std::string getCachePath(const std::string& imagePath) {
  // Replace extension with .pxc (pixel cache)
  size_t dotPos = imagePath.rfind('.');
  if (dotPos != std::string::npos) {
    return imagePath.substr(0, dotPos) + ".pxc";
  }
  return imagePath + ".pxc";
}

inline void rotateLogicalToPhysical(const GfxRenderer::Orientation orientation, int x, int y, int* phyX, int* phyY) {
  switch (orientation) {
    case GfxRenderer::Portrait:
      *phyX = y;
      *phyY = HalDisplay::DISPLAY_HEIGHT - 1 - x;
      break;
    case GfxRenderer::LandscapeClockwise:
      *phyX = HalDisplay::DISPLAY_WIDTH - 1 - x;
      *phyY = HalDisplay::DISPLAY_HEIGHT - 1 - y;
      break;
    case GfxRenderer::PortraitInverted:
      *phyX = HalDisplay::DISPLAY_WIDTH - 1 - y;
      *phyY = x;
      break;
    case GfxRenderer::LandscapeCounterClockwise:
    default:
      *phyX = x;
      *phyY = y;
      break;
  }
}

bool writeOneBitRegionToCache(const GfxRenderer& renderer, const std::string& cachePath, int x, int y, int width, int height) {
  FsFile cacheFile;
  if (!Storage.openFileForWrite("IMG", cachePath, cacheFile)) {
    LOG_ERR("IMG", "Failed to open cache for write: %s", cachePath.c_str());
    return false;
  }

  const uint16_t w16 = static_cast<uint16_t>(width);
  const uint16_t h16 = static_cast<uint16_t>(height);
  if (cacheFile.write(&w16, 2) != 2 || cacheFile.write(&h16, 2) != 2) {
    cacheFile.close();
    LOG_ERR("IMG", "Failed to write cache header: %s", cachePath.c_str());
    return false;
  }

  const uint8_t* fb = renderer.getFrameBuffer();
  if (!fb) {
    cacheFile.close();
    LOG_ERR("IMG", "Framebuffer unavailable while writing cache");
    return false;
  }

  const int bytesPerRow = (width + 3) / 4;  // 2bpp packed
  uint8_t* row = static_cast<uint8_t*>(malloc(bytesPerRow));
  if (!row) {
    cacheFile.close();
    LOG_ERR("IMG", "Failed to allocate cache row buffer");
    return false;
  }

  const auto orientation = renderer.getOrientation();
  for (int oy = 0; oy < height; oy++) {
    memset(row, 0xFF, bytesPerRow);  // default white (stage 3)
    for (int ox = 0; ox < width; ox++) {
      int phyX = 0;
      int phyY = 0;
      rotateLogicalToPhysical(orientation, x + ox, y + oy, &phyX, &phyY);
      if (phyX < 0 || phyX >= HalDisplay::DISPLAY_WIDTH || phyY < 0 || phyY >= HalDisplay::DISPLAY_HEIGHT) {
        continue;
      }
      const uint32_t byteIndex =
          static_cast<uint32_t>(phyY) * HalDisplay::DISPLAY_WIDTH_BYTES + static_cast<uint32_t>(phyX / 8);
      const uint8_t bitPosition = 7 - (phyX % 8);
      const bool isBlack = ((fb[byteIndex] >> bitPosition) & 1u) == 0u;  // drawPixel(true) clears bit
      const uint8_t stage = isBlack ? 0u : 3u;
      const int outByte = ox >> 2;
      const int shift = 6 - ((ox & 3) * 2);
      row[outByte] = static_cast<uint8_t>((row[outByte] & ~(0x03u << shift)) | (stage << shift));
    }
    if (cacheFile.write(row, bytesPerRow) != static_cast<size_t>(bytesPerRow)) {
      free(row);
      cacheFile.close();
      LOG_ERR("IMG", "Failed to write cache row: %s", cachePath.c_str());
      return false;
    }
  }

  free(row);
  cacheFile.flush();
  cacheFile.close();
  return true;
}

bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                     int expectedHeight, bool jpegStyleOneBit = false) {
  FsFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  if (cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) {
    return false;
  }

  // Verify dimensions are close (allow 1 pixel tolerance for rounding differences)
  int widthDiff = abs(cachedWidth - expectedWidth);
  int heightDiff = abs(cachedHeight - expectedHeight);
  if (widthDiff > 1 || heightDiff > 1) {
    LOG_ERR("IMG", "Cache dimension mismatch: %dx%d vs %dx%d", cachedWidth, cachedHeight, expectedWidth,
            expectedHeight);
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
    return false;
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  for (int row = 0; row < cachedHeight; row++) {
    if (cacheFile.read(rowBuffer, bytesPerRow) != bytesPerRow) {
      LOG_ERR("IMG", "Cache read error at row %d", row);
      free(rowBuffer);
      return false;
    }

    const int destY = y + row;
    pw.beginRow(destY);
    for (int col = 0; col < cachedWidth; col++) {
      const int byteIdx = col >> 2;            // col / 4
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      uint8_t pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;
      if (jpegStyleOneBit) {
        // Make cached JPEG playback match JpegRender one-bit behavior.
        // Any non-solid-black stage is treated as white.
        pixelValue = (pixelValue == 0) ? 0 : 3;
      }

      pw.writePixel(x + col, pixelValue);
    }
  }

  free(rowBuffer);
  LOG_DBG("IMG", "Cache render complete");
  return true;
}

}  // namespace

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y) {
  LOG_DBG("IMG", "Rendering image at %d,%d: %s (%dx%d)", x, y, imagePath.c_str(), width, height);

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  // Bounds check render position using logical screen dimensions
  if (x < 0 || y < 0 || x + width > screenWidth || y + height > screenHeight) {
    LOG_ERR("IMG", "Invalid render position: (%d,%d) size (%dx%d) screen (%dx%d)", x, y, width, height, screenWidth,
            screenHeight);
    return;
  }

  // Try to render from cache first.
  std::string cachePath = getCachePath(imagePath);
  const bool isJpeg = FsHelpers::hasJpgExtension(imagePath);
  if (renderFromCache(renderer, cachePath, x, y, width, height, isJpeg)) {
    return;
  }

  // JPEG hybrid path:
  // 1) First draw with JpegRender (requested renderer behavior)
  // 2) Then build legacy .pxc cache through decoder pipeline for subsequent fast loads.
  if (isJpeg) {
    FsFile jpegFile;
    if (!Storage.openFileForRead("IMG", imagePath, jpegFile)) {
      LOG_ERR("IMG", "Image file not found: %s", imagePath.c_str());
      return;
    }
    JpegRender jpeg(renderer);
    const bool rendered = jpeg.oneBit(jpegFile, x, y, width, height, false, true);
    jpegFile.close();
    if (!rendered) {
      LOG_ERR("IMG", "Failed to render JPEG: %s", imagePath.c_str());
      return;
    }

    // Build .pxc directly from what JpegRender drew, so cache playback matches renderer output.
    writeOneBitRegionToCache(renderer, cachePath, x, y, width, height);
    return;
  }

  // No cache - need to decode the image
  // Check if image file exists
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
  config.performanceMode = false;
  config.useExactDimensions = true;  // Use pre-calculated dimensions to avoid rounding mismatches
  config.cachePath = cachePath;      // Enable caching during decode

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    LOG_ERR("IMG", "No decoder found for image: %s", imagePath.c_str());
    return;
  }

  LOG_DBG("IMG", "Using %s decoder", decoder->getFormatName());

  bool success = decoder->decodeToFramebuffer(imagePath, renderer, config);
  if (!success) {
    LOG_ERR("IMG", "Failed to decode image: %s", imagePath.c_str());
    return;
  }

  LOG_DBG("IMG", "Decode successful");
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
