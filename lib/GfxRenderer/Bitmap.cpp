#include "Bitmap.h"

#include <cstdlib>
#include <cstring>

uint16_t Bitmap::readLE16(File& f) {
  const int c0 = f.read();
  const int c1 = f.read();
  const auto b0 = static_cast<uint8_t>(c0 < 0 ? 0 : c0);
  const auto b1 = static_cast<uint8_t>(c1 < 0 ? 0 : c1);
  return static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8);
}

uint32_t Bitmap::readLE32(File& f) {
  const int c0 = f.read();
  const int c1 = f.read();
  const int c2 = f.read();
  const int c3 = f.read();

  const auto b0 = static_cast<uint8_t>(c0 < 0 ? 0 : c0);
  const auto b1 = static_cast<uint8_t>(c1 < 0 ? 0 : c1);
  const auto b2 = static_cast<uint8_t>(c2 < 0 ? 0 : c2);
  const auto b3 = static_cast<uint8_t>(c3 < 0 ? 0 : c3);

  return static_cast<uint32_t>(b0) | (static_cast<uint32_t>(b1) << 8) | (static_cast<uint32_t>(b2) << 16) |
         (static_cast<uint32_t>(b3) << 24);
}

const char* Bitmap::errorToString(BmpReaderError err) {
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
      return "UnsupportedBpp (expected 1, 2, 8, 24, or 32)";
    case BmpReaderError::UnsupportedCompression:
      return "UnsupportedCompression (expected BI_RGB or BI_BITFIELDS for 32bpp)";
    case BmpReaderError::BadDimensions:
      return "BadDimensions";
    case BmpReaderError::PaletteTooLarge:
      return "PaletteTooLarge";

    case BmpReaderError::SeekPixelDataFailed:
      return "SeekPixelDataFailed";
    case BmpReaderError::BufferTooSmall:
      return "BufferTooSmall";

    case BmpReaderError::OomRowBuffer:
      return "OomRowBuffer";
    case BmpReaderError::ShortReadRow:
      return "ShortReadRow";
  }
  return "Unknown";
}

BmpReaderError Bitmap::parseHeaders() {
  if (!file) return BmpReaderError::FileInvalid;
  if (!file.seek(0)) return BmpReaderError::SeekStartFailed;

  // --- BMP FILE HEADER ---
  const uint16_t bfType = readLE16(file);
  if (bfType != 0x4D42) return BmpReaderError::NotBMP;

  file.seek(8, SeekCur);
  bfOffBits = readLE32(file);

  // --- DIB HEADER ---
  const uint32_t biSize = readLE32(file);
  if (biSize < 40) return BmpReaderError::DIBTooSmall;

  width = static_cast<int32_t>(readLE32(file));
  const auto rawHeight = static_cast<int32_t>(readLE32(file));
  topDown = rawHeight < 0;
  height = topDown ? -rawHeight : rawHeight;

  const uint16_t planes = readLE16(file);
  bpp = readLE16(file);
  const uint32_t comp = readLE32(file);
  const bool validBpp = bpp == 1 || bpp == 2 || bpp == 8 || bpp == 24 || bpp == 32;

  if (planes != 1) return BmpReaderError::BadPlanes;
  if (!validBpp) return BmpReaderError::UnsupportedBpp;
  // Allow BI_RGB (0) for all, and BI_BITFIELDS (3) for 32bpp which is common for BGRA masks.
  if (!(comp == 0 || (bpp == 32 && comp == 3))) return BmpReaderError::UnsupportedCompression;

  file.seek(12, SeekCur);  // biSizeImage, biXPelsPerMeter, biYPelsPerMeter
  const uint32_t colorsUsed = readLE32(file);
  if (colorsUsed > 256u) return BmpReaderError::PaletteTooLarge;
  file.seek(4, SeekCur);  // biClrImportant

  if (width <= 0 || height <= 0) return BmpReaderError::BadDimensions;

  // Palette for 8-bit indexed images
  for (int i = 0; i < 256; i++) paletteLum[i] = static_cast<uint8_t>(i);  // default grayscale ramp
  if (colorsUsed > 0) {
    for (uint32_t i = 0; i < colorsUsed; i++) {
      const int b = file.read();
      const int g = file.read();
      const int r = file.read();
      file.seek(1, SeekCur);  // reserved
      const auto bb = static_cast<uint8_t>(b < 0 ? 0 : b);
      const auto gg = static_cast<uint8_t>(g < 0 ? 0 : g);
      const auto rr = static_cast<uint8_t>(r < 0 ? 0 : r);
      paletteLum[i] = static_cast<uint8_t>((77u * rr + 150u * gg + 29u * bb) >> 8);
    }
  }

  if (!file.seek(bfOffBits)) {
    return BmpReaderError::SeekPixelDataFailed;
  }

  return BmpReaderError::Ok;
}

// packed 2bpp output, 0 = black, 1 = dark gray, 2 = light gray, 3 = white
BmpReaderError Bitmap::readRow(uint8_t* data, const size_t dataLen, size_t* read) const {
  // Validate data is long enough to hold a row worth of data
  const size_t outputBytes = (width + 3) / 4;
  if (dataLen < outputBytes) {
    *read = 0;
    return BmpReaderError::BufferTooSmall;
  }

  // setup data to be all black
  memset(data, 0x00, outputBytes);

  const size_t rowBytes = (width * bpp + 31) / 32 * 4;
  const auto rowBuffer = static_cast<uint8_t*>(malloc(rowBytes));
  if (!rowBuffer) {
    *read = 0;
    return BmpReaderError::OomRowBuffer;
  }

  if (file.read(rowBuffer, rowBytes) != rowBytes) {
    free(rowBuffer);
    *read = 0;
    return BmpReaderError::ShortReadRow;
  }

  for (int bmpX = 0; bmpX < width; bmpX++) {
    uint8_t lum;
    if (bpp == 1) {
      const uint8_t byte = rowBuffer[bmpX / 8];
      const uint8_t bit = 7 - (bmpX % 8);
      const bool bitSet = (byte >> bit) & 0x01;
      // In 1bpp BMPs, palette index 0 is conventionally black and index 1 is white.
      lum = bitSet ? 0xFF : 0x00;
    } else if (bpp == 2) {
      const uint8_t byte = rowBuffer[bmpX / 4];
      const uint8_t twobit = 6 - ((bmpX * 2) % 8);
      const uint8_t val = (byte >> twobit) & 0x3;
      lum = paletteLum[val];
    } else if (bpp == 8) {
      const uint8_t idx = rowBuffer[bmpX];
      lum = paletteLum[idx];
    } else {
      // 24 / 32
      const uint8_t* px = &rowBuffer[bmpX * bpp / 8];
      const uint8_t b = px[0];
      const uint8_t g = px[1];
      const uint8_t r = px[2];

      lum = static_cast<uint8_t>((77u * r + 150u * g + 29u * b) >> 8);
    }

    uint8_t color;
    if (lum >= 192) {
      color = 0x3;  // white
    } else if (lum >= 128) {
      color = 0x2;  // light gray
    } else if (lum >= 64) {
      color = 0x1;  // dark gray
    } else {
      color = 0x0;  // black
    }

    data[bmpX / 4] |= (color << (6 - ((bmpX % 4) * 2)));
  }

  free(rowBuffer);
  *read = outputBytes;
  return BmpReaderError::Ok;
}

BmpReaderError Bitmap::rewindToData() const {
  if (!file.seek(bfOffBits)) {
    return BmpReaderError::SeekPixelDataFailed;
  }

  return BmpReaderError::Ok;
}
