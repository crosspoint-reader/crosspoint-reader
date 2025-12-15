#include "BmpReader.h"

#include <cstdlib>
#include <cstring>

uint16_t BmpReader::readLE16(File& f) {
  const int c0 = f.read();
  const int c1 = f.read();
  const uint8_t b0 = (uint8_t)(c0 < 0 ? 0 : c0);
  const uint8_t b1 = (uint8_t)(c1 < 0 ? 0 : c1);
  return (uint16_t)b0 | ((uint16_t)b1 << 8);
}

uint32_t BmpReader::readLE32(File& f) {
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

void BmpReader::freeMonoBitmap(MonoBitmap& bmp) {
  if (bmp.data) {
    free(bmp.data);
    bmp.data = nullptr;
  }
  bmp.width = 0;
  bmp.height = 0;
  bmp.len = 0;
}

const char* BmpReader::errorToString(BmpReaderError err) {
  switch (err) {
    case BmpReaderError::Ok:
      return "Ok";
    case BmpReaderError::FileInvalid:
      return "FileInvalid";
    case BmpReaderError::SeekStartFailed:
      return "SeekStartFailed";
    case BmpReaderError::NotBMP:
      return "NotBMP (missing 'BM')";
    case BmpReaderError::DIBTooSmall:
      return "DIBTooSmall (<40 bytes)";
    case BmpReaderError::BadPlanes:
      return "BadPlanes (!= 1)";
    case BmpReaderError::UnsupportedBpp:
      return "UnsupportedBpp (expected 24, 32 or 1)";
    case BmpReaderError::UnsupportedCompression:
      return "UnsupportedCompression (expected BI_RGB or BI_BITFIELDS for 32bpp)";
    case BmpReaderError::BadDimensions:
      return "BadDimensions";
    case BmpReaderError::SeekPixelDataFailed:
      return "SeekPixelDataFailed";
    case BmpReaderError::OomOutput:
      return "OomOutput";
    case BmpReaderError::OomRowBuffer:
      return "OomRowBuffer";
    case BmpReaderError::ShortReadRow:
      return "ShortReadRow";
  }
  return "Unknown";
}

BmpReaderError BmpReader::read(File& file, MonoBitmap& out, uint8_t threshold) {
  freeMonoBitmap(out);

  if (!file) return BmpReaderError::FileInvalid;
  if (!file.seek(0)) return BmpReaderError::SeekStartFailed;

  // --- BMP FILE HEADER ---
  const uint16_t bfType = readLE16(file);
  if (bfType != 0x4D42) return BmpReaderError::NotBMP;

  (void)readLE32(file);
  (void)readLE16(file);
  (void)readLE16(file);
  const uint32_t bfOffBits = readLE32(file);

  // --- DIB HEADER ---
  const uint32_t biSize = readLE32(file);
  if (biSize < 40) return BmpReaderError::DIBTooSmall;

  const int32_t srcW = (int32_t)readLE32(file);
  int32_t srcHRaw = (int32_t)readLE32(file);
  const uint16_t planes = readLE16(file);
  const uint16_t bpp = readLE16(file);
  const uint32_t comp = readLE32(file);
  const bool is24Bit = (bpp == 24);
  const bool is32Bit = (bpp == 32);
  const bool is8Bit = (bpp == 8);
  const bool is1Bit = (bpp == 1);

  if (planes != 1) return BmpReaderError::BadPlanes;
  if (!is24Bit && !is32Bit && !is8Bit && !is1Bit) return BmpReaderError::UnsupportedBpp;
  // Allow BI_RGB (0) for all, and BI_BITFIELDS (3) for 32bpp which is common for BGRA masks.
  if (!(comp == 0 || (is32Bit && comp == 3))) return BmpReaderError::UnsupportedCompression;

  (void)readLE32(file);  // biSizeImage
  (void)readLE32(file);  // biXPelsPerMeter
  (void)readLE32(file);  // biYPelsPerMeter
  const uint32_t clrUsed = readLE32(file);
  (void)readLE32(file);  // biClrImportant

  if (srcW <= 0) return BmpReaderError::BadDimensions;

  const bool topDown = (srcHRaw < 0);
  const int32_t srcH = topDown ? -srcHRaw : srcHRaw;
  if (srcH <= 0) return BmpReaderError::BadDimensions;

  // Output dimensions after 90° CCW rotation
  out.width = (int)srcH;
  out.height = (int)srcW;

  const size_t outBytesPerRow = (size_t)(out.width + 7) / 8;
  out.len = outBytesPerRow * (size_t)out.height;

  out.data = (uint8_t*)malloc(out.len);
  if (!out.data) return BmpReaderError::OomOutput;
  memset(out.data, 0xFF, out.len);

  // Palette for 8-bit indexed images
  uint8_t paletteLum[256];
  if (is8Bit) {
    for (int i = 0; i < 256; i++) paletteLum[i] = (uint8_t)i;  // default grayscale ramp
    uint32_t paletteCount = (clrUsed == 0) ? 256u : clrUsed;
    if (paletteCount > 256u) paletteCount = 256u;
    for (uint32_t i = 0; i < paletteCount; i++) {
      const int b = file.read();
      const int g = file.read();
      const int r = file.read();
      (void)file.read();  // reserved
      const uint8_t bb = (uint8_t)(b < 0 ? 0 : b);
      const uint8_t gg = (uint8_t)(g < 0 ? 0 : g);
      const uint8_t rr = (uint8_t)(r < 0 ? 0 : r);
      paletteLum[i] = (uint8_t)((77u * rr + 150u * gg + 29u * bb) >> 8);
    }
  }

  // Source row stride (padded to 4 bytes)
  uint32_t bytesPerPixel = 0u;
  if (is8Bit) {
    bytesPerPixel = 1u;
  } else if (is32Bit) {
    bytesPerPixel = 4u;
  } else if (is24Bit) {
    bytesPerPixel = 3u;
  }
  const uint32_t srcBytesPerRow =
      is1Bit ? ((uint32_t)srcW + 7u) / 8u : (uint32_t)srcW * bytesPerPixel;  // bpp==1 ignores bytesPerPixel
  const uint32_t srcRowStride = (srcBytesPerRow + 3u) & ~3u;

  if (!file.seek(bfOffBits)) {
    freeMonoBitmap(out);
    return BmpReaderError::SeekPixelDataFailed;
  }

  uint8_t* rowBuf = (uint8_t*)malloc(srcRowStride);
  if (!rowBuf) {
    freeMonoBitmap(out);
    return BmpReaderError::OomRowBuffer;
  }

  for (int fileRow = 0; fileRow < (int)srcH; fileRow++) {
    if (file.read(rowBuf, srcRowStride) != (int)srcRowStride) {
      free(rowBuf);
      freeMonoBitmap(out);
      return BmpReaderError::ShortReadRow;
    }

    const int srcY = topDown ? fileRow : ((int)srcH - 1 - fileRow);

    for (int srcX = 0; srcX < (int)srcW; srcX++) {
      bool isBlack;
      if (is1Bit) {
        const uint8_t byte = rowBuf[srcX >> 3];
        const uint8_t mask = (uint8_t)(0x80u >> (srcX & 7));
        const bool bitSet = (byte & mask) != 0;
        // In 1bpp BMPs, palette index 0 is conventionally black and index 1 is white.
        isBlack = !bitSet;
      } else if (is8Bit) {
        const uint8_t idx = rowBuf[srcX];
        const uint8_t lum = paletteLum[idx];
        isBlack = (lum < threshold);
      } else {
        const uint8_t* px = &rowBuf[srcX * bytesPerPixel];
        const uint8_t b = px[0];
        const uint8_t g = px[1];
        const uint8_t r = px[2];

        const uint8_t lum = (uint8_t)((77u * r + 150u * g + 29u * b) >> 8);
        isBlack = (lum < threshold);
      }

      // 90° counter-clockwise: (x,y) -> (y, w-1-x)
      const int outX = srcY;
      const int outY = (int)srcW - 1 - srcX;

      setMonoPixel(out.data, out.width, outX, outY, isBlack);
    }
  }

  free(rowBuf);
  return BmpReaderError::Ok;
}
