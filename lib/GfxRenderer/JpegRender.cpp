/**
 * @file JpegRender.cpp
 * @brief 1-bit JPEG decode-to-framebuffer (picojpeg), aligned with inx JpegRender::oneBit.
 */

#include "JpegRender.h"

#include "GfxRenderer.h"
#include <HalStorage.h>
#include <picojpeg.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
struct JpegReadContext {
  FsFile& file;
  uint8_t buffer[512];
  size_t bufferPos;
  size_t bufferFilled;
};

unsigned char jpegReadCallback(unsigned char* pBuf, unsigned char bufSize, unsigned char* pBytesActRead,
                               void* pCallbackData) {
  auto* context = static_cast<JpegReadContext*>(pCallbackData);
  if (!context || !context->file.isOpen()) {
    return PJPG_STREAM_READ_ERROR;
  }
  if (context->bufferPos >= context->bufferFilled) {
    context->bufferFilled = context->file.read(context->buffer, sizeof(context->buffer));
    context->bufferPos = 0;
    if (context->bufferFilled == 0) {
      *pBytesActRead = 0;
      return 0;
    }
  }
  const size_t available = context->bufferFilled - context->bufferPos;
  const size_t toRead = std::min(static_cast<size_t>(bufSize), available);
  memcpy(pBuf, context->buffer + context->bufferPos, toRead);
  context->bufferPos += toRead;
  *pBytesActRead = static_cast<unsigned char>(toRead);
  return 0;
}

bool isUnsupportedJpeg(FsFile& file) {
  const size_t originalPos = file.position();
  file.seek(0);
  uint8_t buf[2];
  bool isProgressive = false;
  while (file.read(buf, 1) == 1) {
    if (buf[0] != 0xFF) continue;
    if (file.read(buf, 1) != 1) break;
    while (buf[0] == 0xFF) {
      if (file.read(buf, 1) != 1) break;
    }
    const uint8_t marker = buf[0];
    if (marker == 0xC2 || marker == 0xC9 || marker == 0xCA) {
      isProgressive = true;
      break;
    }
    if (marker == 0xC0 || marker == 0xC1) {
      isProgressive = false;
      break;
    }
    if (marker != 0xD8 && marker != 0xD9 && marker != 0x01 && !(marker >= 0xD0 && marker <= 0xD7)) {
      if (file.read(buf, 2) != 2) break;
      const uint16_t len = (buf[0] << 8) | buf[1];
      if (len < 2) break;
      file.seek(file.position() + len - 2);
    }
  }
  file.seek(originalPos);
  return isProgressive;
}

inline uint8_t grayFromRgb(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint8_t>((static_cast<uint32_t>(r) * 77u + static_cast<uint32_t>(g) * 150u +
                               static_cast<uint32_t>(b) * 29u) >>
                              8);
}

constexpr int kJpegDitherSolidBlackMax = 32;
constexpr int kJpegDitherSolidWhiteMin = 255 - kJpegDitherSolidBlackMax;

}  // namespace

bool JpegRender::oneBit(FsFile& jpegFile, int x, int y, int targetWidth, int targetHeight, bool cropToFill,
                        bool forceExactSize) const {
  GfxRenderer& renderer = renderer_;
  if (!jpegFile.isOpen() || targetWidth <= 0 || targetHeight <= 0 || isUnsupportedJpeg(jpegFile)) {
    return false;
  }
  JpegReadContext context = {.file = jpegFile, .bufferPos = 0, .bufferFilled = 0};
  pjpeg_image_info_t imageInfo;
  if (pjpeg_decode_init(&imageInfo, jpegReadCallback, &context, 0) != 0) {
    return false;
  }

  int outWidth = imageInfo.m_width;
  int outHeight = imageInfo.m_height;
  uint32_t scaleX_fp = 65536;
  uint32_t scaleY_fp = 65536;
  int srcOffsetX = 0;
  int srcOffsetY = 0;
  int cropSrcWidth = imageInfo.m_width;
  int cropSrcHeight = imageInfo.m_height;

  {
    const float sx = static_cast<float>(targetWidth) / static_cast<float>(imageInfo.m_width);
    const float sy = static_cast<float>(targetHeight) / static_cast<float>(imageInfo.m_height);
    if (forceExactSize) {
      outWidth = targetWidth;
      outHeight = targetHeight;
    } else if (cropToFill) {
      const float scale = std::max(sx, sy);
      cropSrcWidth = std::max(1, static_cast<int>(targetWidth / scale));
      cropSrcHeight = std::max(1, static_cast<int>(targetHeight / scale));
      srcOffsetX = std::max(0, (imageInfo.m_width - cropSrcWidth) / 2);
      srcOffsetY = std::max(0, (imageInfo.m_height - cropSrcHeight) / 2);
      outWidth = targetWidth;
      outHeight = targetHeight;
    } else {
      float scale = std::min(sx, sy);
      if (scale > 1.0f) scale = 1.0f;
      outWidth = std::max(1, static_cast<int>(std::lround(imageInfo.m_width * scale)));
      outHeight = std::max(1, static_cast<int>(std::lround(imageInfo.m_height * scale)));
    }
    scaleX_fp = static_cast<uint32_t>((static_cast<uint64_t>(cropSrcWidth) << 16) / static_cast<uint32_t>(outWidth));
    scaleY_fp = static_cast<uint32_t>((static_cast<uint64_t>(cropSrcHeight) << 16) / static_cast<uint32_t>(outHeight));
  }

  const int drawOffsetX = x + (targetWidth - outWidth) / 2;
  const int drawOffsetY = y + (targetHeight - outHeight) / 2;
  const int srcYEnd = srcOffsetY + cropSrcHeight;

  uint8_t* mcuRowBuffer = static_cast<uint8_t*>(malloc(static_cast<size_t>(imageInfo.m_width) * imageInfo.m_MCUHeight));
  uint32_t* rowAccum = new (std::nothrow) uint32_t[outWidth]();
  uint16_t* rowCount = new (std::nothrow) uint16_t[outWidth]();
  auto* fsErrors = new (std::nothrow) int16_t[static_cast<size_t>(outWidth) * 2]();
  if (!mcuRowBuffer || !rowAccum || !rowCount || !fsErrors) {
    free(mcuRowBuffer);
    delete[] rowAccum;
    delete[] rowCount;
    delete[] fsErrors;
    return false;
  }
  int16_t* errCur = fsErrors;
  int16_t* errNext = fsErrors + outWidth;

  int currentOutY = 0;
  uint32_t nextOutY_srcStart = scaleY_fp;

  for (int mcuY = 0; mcuY < imageInfo.m_MCUSPerCol; mcuY++) {
    for (int mcuX = 0; mcuX < imageInfo.m_MCUSPerRow; mcuX++) {
      if (pjpeg_decode_mcu() != 0) break;
      for (int bY = 0; bY < imageInfo.m_MCUHeight; bY++) {
        for (int bX = 0; bX < imageInfo.m_MCUWidth; bX++) {
          const int pX = mcuX * imageInfo.m_MCUWidth + bX;
          if (pX >= imageInfo.m_width) continue;
          const int off = (bY / 8 * (imageInfo.m_MCUWidth / 8) + bX / 8) * 64 + (bY % 8) * 8 + (bX % 8);
          uint8_t gray = (imageInfo.m_comps == 1) ? imageInfo.m_pMCUBufR[off]
                                                  : grayFromRgb(imageInfo.m_pMCUBufR[off], imageInfo.m_pMCUBufG[off],
                                                                imageInfo.m_pMCUBufB[off]);
          mcuRowBuffer[bY * imageInfo.m_width + pX] = gray;
        }
      }
    }

    for (int yInMcu = 0; yInMcu < imageInfo.m_MCUHeight && (mcuY * imageInfo.m_MCUHeight + yInMcu) < imageInfo.m_height;
         yInMcu++) {
      const int srcY = mcuY * imageInfo.m_MCUHeight + yInMcu;
      if (srcY < srcOffsetY || srcY >= srcYEnd) continue;
      const uint8_t* srcRow = mcuRowBuffer + yInMcu * imageInfo.m_width;
      for (int ox = 0; ox < outWidth; ox++) {
        const int sx = srcOffsetX + static_cast<int>((static_cast<uint64_t>(ox) * scaleX_fp) >> 16);
        if (sx >= srcOffsetX && sx < srcOffsetX + cropSrcWidth && sx < imageInfo.m_width) {
          rowAccum[ox] += srcRow[sx];
          rowCount[ox]++;
        }
      }
      if ((static_cast<uint32_t>(srcY + 1) << 16) >= nextOutY_srcStart && currentOutY < outHeight) {
        const int screenY = drawOffsetY + currentOutY;
        for (int ox = 0; ox < outWidth; ox++) {
          const int gray = rowCount[ox] ? static_cast<int>(rowAccum[ox] / rowCount[ox]) : 0;
          int corrected = gray + static_cast<int>(errCur[ox]);
          if (corrected < 0) corrected = 0;
          if (corrected > 255) corrected = 255;

          int q;
          int err;
          if (gray <= kJpegDitherSolidBlackMax) {
            q = 0;
            err = 0;
          } else if (gray >= kJpegDitherSolidWhiteMin) {
            q = 255;
            err = 0;
          } else {
            q = corrected < 128 ? 0 : 255;
            err = corrected - q;
            if (ox + 1 < outWidth) errCur[ox + 1] = static_cast<int16_t>(errCur[ox + 1] + (err * 7) / 16);
            errNext[ox] = static_cast<int16_t>(errNext[ox] + (err * 5) / 16);
            if (ox > 0) errNext[ox - 1] = static_cast<int16_t>(errNext[ox - 1] + (err * 3) / 16);
            if (ox + 1 < outWidth) errNext[ox + 1] = static_cast<int16_t>(errNext[ox + 1] + (err * 1) / 16);
          }

          const int px = drawOffsetX + ox;
          if (q == 0) {
            renderer.drawPixel(px, screenY, true);
          }
        }
        int16_t* t = errCur;
        errCur = errNext;
        errNext = t;
        memset(errNext, 0, static_cast<size_t>(outWidth) * sizeof(int16_t));
        currentOutY++;
        memset(rowAccum, 0, static_cast<size_t>(outWidth) * sizeof(uint32_t));
        memset(rowCount, 0, static_cast<size_t>(outWidth) * sizeof(uint16_t));
        nextOutY_srcStart = static_cast<uint32_t>(currentOutY + 1) * scaleY_fp;
      }
    }
  }

  free(mcuRowBuffer);
  delete[] rowAccum;
  delete[] rowCount;
  delete[] fsErrors;
  return currentOutY > 0;
}

bool JpegRender::fromPath(const std::string& path, int x, int y, int targetWidth, int targetHeight,
                          bool cropToFill, bool forceExactSize) const {
  FsFile file;
  if (!Storage.openFileForRead("JRG", path, file)) {
    return false;
  }
  const bool ok = oneBit(file, x, y, targetWidth, targetHeight, cropToFill, forceExactSize);
  file.close();
  return ok;
}

bool JpegRender::getDimensions(FsFile& jpegFile, int* outW, int* outH) {
  if (!outW || !outH || !jpegFile.isOpen()) {
    return false;
  }
  *outW = 0;
  *outH = 0;
  const size_t pos = jpegFile.position();
  jpegFile.seek(0);
  JpegReadContext context = {.file = jpegFile, .bufferPos = 0, .bufferFilled = 0};
  pjpeg_image_info_t imageInfo;
  const bool ok = pjpeg_decode_init(&imageInfo, jpegReadCallback, &context, 0) == 0;
  if (ok) {
    *outW = imageInfo.m_width;
    *outH = imageInfo.m_height;
  }
  jpegFile.seek(pos);
  return ok && *outW > 0 && *outH > 0;
}

bool JpegRender::getDimensions(const std::string& path, int* outW, int* outH) {
  FsFile file;
  if (!Storage.openFileForRead("JRG", path, file)) {
    return false;
  }
  const bool ok = getDimensions(file, outW, outH);
  file.close();
  return ok;
}
