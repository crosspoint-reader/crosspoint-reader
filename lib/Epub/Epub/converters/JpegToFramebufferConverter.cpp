#include "JpegToFramebufferConverter.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <picojpeg.h>

#include <cstdio>
#include <cstring>

#include "DitherUtils.h"
#include "PixelCache.h"

struct JpegContext {
  FsFile& file;
  uint8_t buffer[512];
  size_t bufferPos;
  size_t bufferFilled;
  JpegContext(FsFile& f) : file(f), bufferPos(0), bufferFilled(0) {}
};

bool JpegToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  FsFile file;
  if (!Storage.openFileForRead("JPG", imagePath, file)) {
    LOG_ERR("JPG", "Failed to open file for dimensions: %s", imagePath.c_str());
    return false;
  }

  JpegContext context(file);
  pjpeg_image_info_t imageInfo;

  int status = pjpeg_decode_init(&imageInfo, jpegReadCallback, &context, 0);
  file.close();

  if (status != 0) {
    LOG_ERR("JPG", "Failed to init JPEG for dimensions: %d", status);
    return false;
  }

  out.width = imageInfo.m_width;
  out.height = imageInfo.m_height;
  LOG_DBG("JPG", "Image dimensions: %dx%d", out.width, out.height);
  return true;
}

bool JpegToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                     const RenderConfig& config) {
  LOG_DBG("JPG", "Decoding JPEG: %s", imagePath.c_str());

  FsFile file;
  if (!Storage.openFileForRead("JPG", imagePath, file)) {
    LOG_ERR("JPG", "Failed to open file: %s", imagePath.c_str());
    return false;
  }

  JpegContext context(file);
  pjpeg_image_info_t imageInfo;

  int status = pjpeg_decode_init(&imageInfo, jpegReadCallback, &context, 0);
  if (status != 0) {
    LOG_ERR("JPG", "picojpeg init failed: %d", status);
    file.close();
    return false;
  }

  if (!validateImageDimensions(imageInfo.m_width, imageInfo.m_height, "JPEG")) {
    file.close();
    return false;
  }

  // Calculate output dimensions
  int destWidth, destHeight;
  float scale;

  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    // Use exact dimensions as specified (avoids rounding mismatches with pre-calculated sizes)
    destWidth = config.maxWidth;
    destHeight = config.maxHeight;
    scale = (float)destWidth / imageInfo.m_width;
  } else {
    // Calculate scale factor to fit within maxWidth/maxHeight
    float scaleX = (config.maxWidth > 0 && imageInfo.m_width > config.maxWidth)
                       ? (float)config.maxWidth / imageInfo.m_width
                       : 1.0f;
    float scaleY = (config.maxHeight > 0 && imageInfo.m_height > config.maxHeight)
                       ? (float)config.maxHeight / imageInfo.m_height
                       : 1.0f;
    scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (scale > 1.0f) scale = 1.0f;

    destWidth = (int)(imageInfo.m_width * scale);
    destHeight = (int)(imageInfo.m_height * scale);
  }

  LOG_DBG("JPG", "JPEG %dx%d -> %dx%d (scale %.2f), scan type: %d, MCU: %dx%d", imageInfo.m_width, imageInfo.m_height,
          destWidth, destHeight, scale, imageInfo.m_scanType, imageInfo.m_MCUWidth, imageInfo.m_MCUHeight);

  if (!imageInfo.m_pMCUBufR || !imageInfo.m_pMCUBufG || !imageInfo.m_pMCUBufB) {
    LOG_ERR("JPG", "Null buffer pointers in imageInfo");
    file.close();
    return false;
  }

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  // Allocate pixel cache if cachePath is provided
  PixelCache cache;
  bool caching = !config.cachePath.empty();
  if (caching) {
    if (!cache.allocate(destWidth, destHeight, config.x, config.y)) {
      LOG_ERR("JPG", "Failed to allocate cache buffer, continuing without caching");
      caching = false;
    }
  }

  int mcuX = 0;
  int mcuY = 0;

  while (mcuY < imageInfo.m_MCUSPerCol) {
    status = pjpeg_decode_mcu();
    if (status == PJPG_NO_MORE_BLOCKS) {
      break;
    }
    if (status != 0) {
      LOG_ERR("JPG", "MCU decode failed: %d", status);
      file.close();
      return false;
    }

    // Source position in image coordinates
    int srcStartX = mcuX * imageInfo.m_MCUWidth;
    int srcStartY = mcuY * imageInfo.m_MCUHeight;

    switch (imageInfo.m_scanType) {
      case PJPG_GRAYSCALE:
        for (int row = 0; row < 8; row++) {
          int srcY = srcStartY + row;
          int destY = config.y + (int)(srcY * scale);
          if (destY >= screenHeight || destY >= config.y + destHeight) continue;
          for (int col = 0; col < 8; col++) {
            int srcX = srcStartX + col;
            int destX = config.x + (int)(srcX * scale);
            if (destX >= screenWidth || destX >= config.x + destWidth) continue;
            uint8_t gray = imageInfo.m_pMCUBufR[row * 8 + col];
            uint8_t dithered = config.useDithering ? applyBayerDither4Level(gray, destX, destY) : gray / 85;
            if (dithered > 3) dithered = 3;
            drawPixelWithRenderMode(renderer, destX, destY, dithered);
            if (caching) cache.setPixel(destX, destY, dithered);
          }
        }
        break;

      case PJPG_YH1V1:
        for (int row = 0; row < 8; row++) {
          int srcY = srcStartY + row;
          int destY = config.y + (int)(srcY * scale);
          if (destY >= screenHeight || destY >= config.y + destHeight) continue;
          for (int col = 0; col < 8; col++) {
            int srcX = srcStartX + col;
            int destX = config.x + (int)(srcX * scale);
            if (destX >= screenWidth || destX >= config.x + destWidth) continue;
            uint8_t r = imageInfo.m_pMCUBufR[row * 8 + col];
            uint8_t g = imageInfo.m_pMCUBufG[row * 8 + col];
            uint8_t b = imageInfo.m_pMCUBufB[row * 8 + col];
            uint8_t gray = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
            uint8_t dithered = config.useDithering ? applyBayerDither4Level(gray, destX, destY) : gray / 85;
            if (dithered > 3) dithered = 3;
            drawPixelWithRenderMode(renderer, destX, destY, dithered);
            if (caching) cache.setPixel(destX, destY, dithered);
          }
        }
        break;

      case PJPG_YH2V1:
        for (int row = 0; row < 8; row++) {
          int srcY = srcStartY + row;
          int destY = config.y + (int)(srcY * scale);
          if (destY >= screenHeight || destY >= config.y + destHeight) continue;
          for (int col = 0; col < 16; col++) {
            int srcX = srcStartX + col;
            int destX = config.x + (int)(srcX * scale);
            if (destX >= screenWidth || destX >= config.x + destWidth) continue;
            int blockIndex = (col < 8) ? 0 : 1;
            int pixelIndex = row * 8 + (col % 8);
            uint8_t r = imageInfo.m_pMCUBufR[blockIndex * 64 + pixelIndex];
            uint8_t g = imageInfo.m_pMCUBufG[blockIndex * 64 + pixelIndex];
            uint8_t b = imageInfo.m_pMCUBufB[blockIndex * 64 + pixelIndex];
            uint8_t gray = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
            uint8_t dithered = config.useDithering ? applyBayerDither4Level(gray, destX, destY) : gray / 85;
            if (dithered > 3) dithered = 3;
            drawPixelWithRenderMode(renderer, destX, destY, dithered);
            if (caching) cache.setPixel(destX, destY, dithered);
          }
        }
        break;

      case PJPG_YH1V2:
        for (int row = 0; row < 16; row++) {
          int srcY = srcStartY + row;
          int destY = config.y + (int)(srcY * scale);
          if (destY >= screenHeight || destY >= config.y + destHeight) continue;
          for (int col = 0; col < 8; col++) {
            int srcX = srcStartX + col;
            int destX = config.x + (int)(srcX * scale);
            if (destX >= screenWidth || destX >= config.x + destWidth) continue;
            int blockIndex = (row < 8) ? 0 : 1;
            int pixelIndex = (row % 8) * 8 + col;
            uint8_t r = imageInfo.m_pMCUBufR[blockIndex * 128 + pixelIndex];
            uint8_t g = imageInfo.m_pMCUBufG[blockIndex * 128 + pixelIndex];
            uint8_t b = imageInfo.m_pMCUBufB[blockIndex * 128 + pixelIndex];
            uint8_t gray = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
            uint8_t dithered = config.useDithering ? applyBayerDither4Level(gray, destX, destY) : gray / 85;
            if (dithered > 3) dithered = 3;
            drawPixelWithRenderMode(renderer, destX, destY, dithered);
            if (caching) cache.setPixel(destX, destY, dithered);
          }
        }
        break;

      case PJPG_YH2V2:
        for (int row = 0; row < 16; row++) {
          int srcY = srcStartY + row;
          int destY = config.y + (int)(srcY * scale);
          if (destY >= screenHeight || destY >= config.y + destHeight) continue;
          for (int col = 0; col < 16; col++) {
            int srcX = srcStartX + col;
            int destX = config.x + (int)(srcX * scale);
            if (destX >= screenWidth || destX >= config.x + destWidth) continue;
            int blockX = (col < 8) ? 0 : 1;
            int blockY = (row < 8) ? 0 : 1;
            int blockIndex = blockY * 2 + blockX;
            int pixelIndex = (row % 8) * 8 + (col % 8);
            int blockOffset = blockIndex * 64;
            uint8_t r = imageInfo.m_pMCUBufR[blockOffset + pixelIndex];
            uint8_t g = imageInfo.m_pMCUBufG[blockOffset + pixelIndex];
            uint8_t b = imageInfo.m_pMCUBufB[blockOffset + pixelIndex];
            uint8_t gray = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
            uint8_t dithered = config.useDithering ? applyBayerDither4Level(gray, destX, destY) : gray / 85;
            if (dithered > 3) dithered = 3;
            drawPixelWithRenderMode(renderer, destX, destY, dithered);
            if (caching) cache.setPixel(destX, destY, dithered);
          }
        }
        break;
    }

    mcuX++;
    if (mcuX >= imageInfo.m_MCUSPerRow) {
      mcuX = 0;
      mcuY++;
    }
  }

  LOG_DBG("JPG", "Decoding complete");
  file.close();

  // Write cache file if caching was enabled
  if (caching) {
    cache.writeToFile(config.cachePath);
  }

  return true;
}

unsigned char JpegToFramebufferConverter::jpegReadCallback(unsigned char* pBuf, unsigned char buf_size,
                                                           unsigned char* pBytes_actually_read, void* pCallback_data) {
  JpegContext* context = reinterpret_cast<JpegContext*>(pCallback_data);

  if (context->bufferPos >= context->bufferFilled) {
    int readCount = context->file.read(context->buffer, sizeof(context->buffer));
    if (readCount <= 0) {
      *pBytes_actually_read = 0;
      return 0;
    }
    context->bufferFilled = readCount;
    context->bufferPos = 0;
  }

  unsigned int bytesAvailable = context->bufferFilled - context->bufferPos;
  unsigned int bytesToCopy = (bytesAvailable < buf_size) ? bytesAvailable : buf_size;

  memcpy(pBuf, &context->buffer[context->bufferPos], bytesToCopy);
  context->bufferPos += bytesToCopy;
  *pBytes_actually_read = bytesToCopy;

  return 0;
}

bool JpegToFramebufferConverter::supportsFormat(const std::string& extension) {
  std::string ext = extension;
  for (auto& c : ext) {
    c = tolower(c);
  }
  return (ext == ".jpg" || ext == ".jpeg");
}
