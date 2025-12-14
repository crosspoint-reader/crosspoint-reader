#include "BmpToMono.h"

#include <cstdlib>
#include <cstring>

uint16_t BmpToMono::readLE16(File& f) {
  const int c0 = f.read();
  const int c1 = f.read();
  const uint8_t b0 = (uint8_t)(c0 < 0 ? 0 : c0);
  const uint8_t b1 = (uint8_t)(c1 < 0 ? 0 : c1);
  return (uint16_t)b0 | ((uint16_t)b1 << 8);
}

uint32_t BmpToMono::readLE32(File& f) {
  const int c0 = f.read();
  const int c1 = f.read();
  const int c2 = f.read();
  const int c3 = f.read();

  const uint8_t b0 = (uint8_t)(c0 < 0 ? 0 : c0);
  const uint8_t b1 = (uint8_t)(c1 < 0 ? 0 : c1);
  const uint8_t b2 = (uint8_t)(c2 < 0 ? 0 : c2);
  const uint8_t b3 = (uint8_t)(c3 < 0 ? 0 : c3);

  return (uint32_t)b0 | ((uint32_t)b1 << 8) | ((uint32_t)b2 << 16) | ((uint32_t)b3 << 24);
}

void BmpToMono::freeMonoBitmap(MonoBitmap& bmp) {
  if (bmp.data) {
    free(bmp.data);
    bmp.data = nullptr;
  }
  bmp.width = 0;
  bmp.height = 0;
  bmp.len = 0;
}

const char* BmpToMono::errorToString(BmpToMonoError err) {
  switch (err) {
    case BmpToMonoError::Ok:
      return "Ok";
    case BmpToMonoError::FileInvalid:
      return "FileInvalid";
    case BmpToMonoError::SeekStartFailed:
      return "SeekStartFailed";
    case BmpToMonoError::NotBMP:
      return "NotBMP (missing 'BM')";
    case BmpToMonoError::DIBTooSmall:
      return "DIBTooSmall (<40 bytes)";
    case BmpToMonoError::BadPlanes:
      return "BadPlanes (!= 1)";
    case BmpToMonoError::UnsupportedBpp:
      return "UnsupportedBpp (expected 24)";
    case BmpToMonoError::UnsupportedCompression:
      return "UnsupportedCompression (expected BI_RGB)";
    case BmpToMonoError::BadDimensions:
      return "BadDimensions";
    case BmpToMonoError::SeekPixelDataFailed:
      return "SeekPixelDataFailed";
    case BmpToMonoError::OomOutput:
      return "OomOutput";
    case BmpToMonoError::OomRowBuffer:
      return "OomRowBuffer";
    case BmpToMonoError::ShortReadRow:
      return "ShortReadRow";
  }
  return "Unknown";
}

BmpToMonoError BmpToMono::convert24BitRotate90CCW(File& file, MonoBitmap& out, uint8_t threshold) {
  return convert24BitImpl(file, out, threshold, true);
}

BmpToMonoError BmpToMono::convert24BitImpl(File& f, MonoBitmap& out, uint8_t threshold, bool rotate90CCW) {
  freeMonoBitmap(out);

  if (!f) return BmpToMonoError::FileInvalid;
  if (!f.seek(0)) return BmpToMonoError::SeekStartFailed;

  // --- BMP FILE HEADER ---
  const uint16_t bfType = readLE16(f);
  if (bfType != 0x4D42) return BmpToMonoError::NotBMP;

  (void)readLE32(f);
  (void)readLE16(f);
  (void)readLE16(f);
  const uint32_t bfOffBits = readLE32(f);

  // --- DIB HEADER ---
  const uint32_t biSize = readLE32(f);
  if (biSize < 40) return BmpToMonoError::DIBTooSmall;

  const int32_t srcW = (int32_t)readLE32(f);
  int32_t srcHRaw = (int32_t)readLE32(f);
  const uint16_t planes = readLE16(f);
  const uint16_t bpp = readLE16(f);
  const uint32_t comp = readLE32(f);

  if (planes != 1) return BmpToMonoError::BadPlanes;
  if (bpp != 24) return BmpToMonoError::UnsupportedBpp;
  if (comp != 0) return BmpToMonoError::UnsupportedCompression;

  (void)readLE32(f);
  (void)readLE32(f);
  (void)readLE32(f);
  (void)readLE32(f);
  (void)readLE32(f);

  if (srcW <= 0) return BmpToMonoError::BadDimensions;

  const bool topDown = (srcHRaw < 0);
  const int32_t srcH = topDown ? -srcHRaw : srcHRaw;
  if (srcH <= 0) return BmpToMonoError::BadDimensions;

  // Output dimensions
  out.width = rotate90CCW ? (int)srcH : (int)srcW;
  out.height = rotate90CCW ? (int)srcW : (int)srcH;

  const size_t outBytesPerRow = (size_t)(out.width + 7) / 8;
  out.len = outBytesPerRow * (size_t)out.height;

  out.data = (uint8_t*)malloc(out.len);
  if (!out.data) return BmpToMonoError::OomOutput;
  memset(out.data, 0xFF, out.len);

  // Source row stride (padded to 4 bytes)
  const uint32_t srcBytesPerRow24 = (uint32_t)srcW * 3u;
  const uint32_t srcRowStride = (srcBytesPerRow24 + 3u) & ~3u;

  if (!f.seek(bfOffBits)) {
    freeMonoBitmap(out);
    return BmpToMonoError::SeekPixelDataFailed;
  }

  uint8_t* rowBuf = (uint8_t*)malloc(srcRowStride);
  if (!rowBuf) {
    freeMonoBitmap(out);
    return BmpToMonoError::OomRowBuffer;
  }

  for (int fileRow = 0; fileRow < (int)srcH; fileRow++) {
    if (f.read(rowBuf, srcRowStride) != (int)srcRowStride) {
      free(rowBuf);
      freeMonoBitmap(out);
      return BmpToMonoError::ShortReadRow;
    }

    const int srcY = topDown ? fileRow : ((int)srcH - 1 - fileRow);

    for (int srcX = 0; srcX < (int)srcW; srcX++) {
      const uint8_t b = rowBuf[srcX * 3 + 0];
      const uint8_t g = rowBuf[srcX * 3 + 1];
      const uint8_t r = rowBuf[srcX * 3 + 2];

      const uint8_t lum = (uint8_t)((77u * r + 150u * g + 29u * b) >> 8);
      bool isBlack = (lum < threshold);

      int outX, outY;
      if (!rotate90CCW) {
        outX = srcX;
        outY = srcY;
      } else {
        // 90° counter-clockwise: (x,y) -> (y, w-1-x)
        outX = srcY;
        outY = (int)srcW - 1 - srcX;
      }

      setMonoPixel(out.data, out.width, outX, outY, isBlack);
    }
  }

  free(rowBuf);
  return BmpToMonoError::Ok;
}