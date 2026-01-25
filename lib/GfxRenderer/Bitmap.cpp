#include "Bitmap.h"
#include "BitmapHelpers.h"
#include <cstdlib>
#include <cstring>

// ============================================================================
// IMAGE PROCESSING OPTIONS
// ============================================================================
constexpr bool USE_ATKINSON = true;

Bitmap::~Bitmap() {
  delete[] errorCurRow;
  delete[] errorNextRow;
  delete atkinsonDitherer;
  delete fsDitherer;
}

// ===================================
// IO Helpers
// ===================================

int Bitmap::readByte() const {
  if (file && *file) {
    return file->read();
  } else if (memoryBuffer) {
    if (bufferPos < memorySize) {
      return memoryBuffer[bufferPos++];
    }
    return -1;
  }
  return -1;
}

size_t Bitmap::readBytes(void *buf, size_t count) const {
  if (file && *file) {
    return file->read(buf, count);
  } else if (memoryBuffer) {
    size_t available = memorySize - bufferPos;
    if (count > available)
      count = available;
    memcpy(buf, memoryBuffer + bufferPos, count);
    bufferPos += count;
    return count;
  }
  return 0;
}

bool Bitmap::seekSet(uint32_t pos) const {
  if (file && *file) {
    return file->seek(pos);
  } else if (memoryBuffer) {
    if (pos <= memorySize) {
      bufferPos = pos;
      return true;
    }
    return false;
  }
  return false;
}

bool Bitmap::seekCur(int32_t offset) const {
  if (file && *file) {
    return file->seekCur(offset);
  } else if (memoryBuffer) {
    if (bufferPos + offset <= memorySize) {
      bufferPos += offset;
      return true;
    }
    return false;
  }
  return false;
}

uint16_t Bitmap::readLE16() {
  const int c0 = readByte();
  const int c1 = readByte();
  return static_cast<uint16_t>(c0 & 0xFF) |
         (static_cast<uint16_t>(c1 & 0xFF) << 8);
}

uint32_t Bitmap::readLE32() {
  const int c0 = readByte();
  const int c1 = readByte();
  const int c2 = readByte();
  const int c3 = readByte();
  return static_cast<uint32_t>(c0 & 0xFF) |
         (static_cast<uint32_t>(c1 & 0xFF) << 8) |
         (static_cast<uint32_t>(c2 & 0xFF) << 16) |
         (static_cast<uint32_t>(c3 & 0xFF) << 24);
}

const char *Bitmap::errorToString(BmpReaderError err) {
  switch (err) {
  case BmpReaderError::Ok:
    return "Ok";
  case BmpReaderError::FileInvalid:
    return "FileInvalid";
  case BmpReaderError::SeekStartFailed:
    return "SeekStartFailed";
  case BmpReaderError::NotBMP:
    return "NotBMP";
  case BmpReaderError::DIBTooSmall:
    return "DIBTooSmall";
  case BmpReaderError::BadPlanes:
    return "BadPlanes";
  case BmpReaderError::UnsupportedBpp:
    return "UnsupportedBpp";
  case BmpReaderError::UnsupportedCompression:
    return "UnsupportedCompression";
  case BmpReaderError::BadDimensions:
    return "BadDimensions";
  case BmpReaderError::ImageTooLarge:
    return "ImageTooLarge";
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
  if (!file && !memoryBuffer)
    return BmpReaderError::FileInvalid;
  if (!seekSet(0))
    return BmpReaderError::SeekStartFailed;

  const uint16_t bfType = readLE16();
  if (bfType != 0x4D42)
    return BmpReaderError::NotBMP;

  seekCur(8);
  bfOffBits = readLE32();

  const uint32_t biSize = readLE32();
  if (biSize < 40)
    return BmpReaderError::DIBTooSmall;

  width = static_cast<int32_t>(readLE32());
  const auto rawHeight = static_cast<int32_t>(readLE32());
  topDown = rawHeight < 0;
  height = topDown ? -rawHeight : rawHeight;

  const uint16_t planes = readLE16();
  bpp = readLE16();
  const uint32_t comp = readLE32();
  const bool validBpp =
      bpp == 1 || bpp == 2 || bpp == 8 || bpp == 24 || bpp == 32;

  if (planes != 1)
    return BmpReaderError::BadPlanes;
  if (!validBpp)
    return BmpReaderError::UnsupportedBpp;
  if (!(comp == 0 || (bpp == 32 && comp == 3)))
    return BmpReaderError::UnsupportedCompression;

  seekCur(12);
  const uint32_t colorsUsed = readLE32();
  if (colorsUsed > 256u)
    return BmpReaderError::PaletteTooLarge;
  seekCur(4);

  if (width <= 0 || height <= 0)
    return BmpReaderError::BadDimensions;

  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;
  if (width > MAX_IMAGE_WIDTH || height > MAX_IMAGE_HEIGHT) {
    return BmpReaderError::ImageTooLarge;
  }

  rowBytes = (width * bpp + 31) / 32 * 4;

  for (int i = 0; i < 256; i++)
    paletteLum[i] = static_cast<uint8_t>(i);
  if (colorsUsed > 0) {
    for (uint32_t i = 0; i < colorsUsed; i++) {
      uint8_t rgb[4];
      readBytes(rgb, 4);
      paletteLum[i] = (77u * rgb[2] + 150u * rgb[1] + 29u * rgb[0]) >> 8;
    }
  }

  if (!seekSet(bfOffBits))
    return BmpReaderError::SeekPixelDataFailed;

  if (bpp > 2 && dithering) {
    if (USE_ATKINSON) {
      atkinsonDitherer = new AtkinsonDitherer(width);
    } else {
      fsDitherer = new FloydSteinbergDitherer(width);
    }
  }

  return BmpReaderError::Ok;
}

BmpReaderError Bitmap::readNextRow(uint8_t *data, uint8_t *rowBuffer) const {
  if (readBytes(rowBuffer, rowBytes) != (size_t)rowBytes)
    return BmpReaderError::ShortReadRow;

  prevRowY += 1;
  uint8_t *outPtr = data;
  uint8_t currentOutByte = 0;
  int bitShift = 6;
  int currentX = 0;

  auto packPixel = [&](const uint8_t lum) {
    uint8_t color;
    if (atkinsonDitherer) {
      color = atkinsonDitherer->processPixel(lum, currentX);
    } else if (fsDitherer) {
      color = fsDitherer->processPixel(lum, currentX);
    } else {
      if (bpp > 2) {
        color = quantize(adjustPixel(lum), currentX, prevRowY);
      } else {
        color = static_cast<uint8_t>(lum >> 6);
      }
    }
    currentOutByte |= (color << bitShift);
    if (bitShift == 0) {
      *outPtr++ = currentOutByte;
      currentOutByte = 0;
      bitShift = 6;
    } else {
      bitShift -= 2;
    }
    currentX++;
  };

  switch (bpp) {
  case 32: {
    const uint8_t *p = rowBuffer;
    for (int x = 0; x < width; x++) {
      uint8_t lum = (77u * p[2] + 150u * p[1] + 29u * p[0]) >> 8;
      packPixel(lum);
      p += 4;
    }
    break;
  }
  case 24: {
    const uint8_t *p = rowBuffer;
    for (int x = 0; x < width; x++) {
      uint8_t lum = (77u * p[2] + 150u * p[1] + 29u * p[0]) >> 8;
      packPixel(lum);
      p += 3;
    }
    break;
  }
  case 8: {
    for (int x = 0; x < width; x++)
      packPixel(paletteLum[rowBuffer[x]]);
    break;
  }
  case 2: {
    for (int x = 0; x < width; x++) {
      uint8_t lum =
          paletteLum[(rowBuffer[x >> 2] >> (6 - ((x & 3) * 2))) & 0x03];
      packPixel(lum);
    }
    break;
  }
  case 1: {
    for (int x = 0; x < width; x++) {
      const uint8_t palIndex = (rowBuffer[x >> 3] & (0x80 >> (x & 7))) ? 1 : 0;
      packPixel(paletteLum[palIndex]);
    }
    break;
  }
  default:
    return BmpReaderError::UnsupportedBpp;
  }

  if (atkinsonDitherer)
    atkinsonDitherer->nextRow();
  else if (fsDitherer)
    fsDitherer->nextRow();

  if (bitShift != 6)
    *outPtr = currentOutByte;
  return BmpReaderError::Ok;
}

BmpReaderError Bitmap::rewindToData() const {
  if (!seekSet(bfOffBits))
    return BmpReaderError::SeekPixelDataFailed;
  if (fsDitherer)
    fsDitherer->reset();
  if (atkinsonDitherer)
    atkinsonDitherer->reset();
  return BmpReaderError::Ok;
}
