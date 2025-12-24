#include "Bitmap.h"

#include <cstdlib>
#include <cstring>

// ============================================================================
// IMAGE PROCESSING OPTIONS - Toggle these to test different configurations
// ============================================================================
// Note: For cover images, dithering is done in JpegToBmpConverter.cpp
// This file handles BMP reading - use simple quantization to avoid double-dithering
constexpr bool USE_FLOYD_STEINBERG = false;  // Disabled - dithering done at JPEG conversion
constexpr bool USE_NOISE_DITHERING = false;  // Hash-based noise dithering
// Brightness adjustments:
constexpr bool USE_BRIGHTNESS = false;    // true: apply brightness/gamma adjustments
constexpr int BRIGHTNESS_BOOST = 20;      // Brightness offset (0-50), only if USE_BRIGHTNESS=true
constexpr bool GAMMA_CORRECTION = false;  // Gamma curve, only if USE_BRIGHTNESS=true
// ============================================================================

// Integer approximation of gamma correction (brightens midtones)
static inline int applyGamma(int gray) {
  if (!GAMMA_CORRECTION) return gray;
  const int product = gray * 255;
  int x = gray;
  if (x > 0) {
    x = (x + product / x) >> 1;
    x = (x + product / x) >> 1;
  }
  return x > 255 ? 255 : x;
}

// Simple quantization without dithering - just divide into 4 levels
static inline uint8_t quantizeSimple(int gray) {
  if (USE_BRIGHTNESS) {
    gray += BRIGHTNESS_BOOST;
    if (gray > 255) gray = 255;
    gray = applyGamma(gray);
  }
  return static_cast<uint8_t>(gray >> 6);
}

// Hash-based noise dithering - survives downsampling without moiré artifacts
static inline uint8_t quantizeNoise(int gray, int x, int y) {
  if (USE_BRIGHTNESS) {
    gray += BRIGHTNESS_BOOST;
    if (gray > 255) gray = 255;
    gray = applyGamma(gray);
  }

  uint32_t hash = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
  hash = (hash ^ (hash >> 13)) * 1274126177u;
  const int threshold = static_cast<int>(hash >> 24);

  const int scaled = gray * 3;
  if (scaled < 255) {
    return (scaled + threshold >= 255) ? 1 : 0;
  } else if (scaled < 510) {
    return ((scaled - 255) + threshold >= 255) ? 2 : 1;
  } else {
    return ((scaled - 510) + threshold >= 255) ? 3 : 2;
  }
}

// Main quantization function
static inline uint8_t quantize(int gray, int x, int y) {
  if (USE_NOISE_DITHERING) {
    return quantizeNoise(gray, x, y);
  } else {
    return quantizeSimple(gray);
  }
}

// Floyd-Steinberg quantization with error diffusion and serpentine scanning
// Returns 2-bit value (0-3) and updates error buffers
static inline uint8_t quantizeFloydSteinberg(int gray, int x, int width, int16_t* errorCurRow, int16_t* errorNextRow,
                                             bool reverseDir) {
  // Add accumulated error to this pixel
  int adjusted = gray + errorCurRow[x + 1];

  // Clamp to valid range
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;

  // Quantize to 4 levels (0, 85, 170, 255)
  uint8_t quantized;
  int quantizedValue;
  if (adjusted < 43) {
    quantized = 0;
    quantizedValue = 0;
  } else if (adjusted < 128) {
    quantized = 1;
    quantizedValue = 85;
  } else if (adjusted < 213) {
    quantized = 2;
    quantizedValue = 170;
  } else {
    quantized = 3;
    quantizedValue = 255;
  }

  // Calculate error
  int error = adjusted - quantizedValue;

  // Distribute error to neighbors (serpentine: direction-aware)
  if (!reverseDir) {
    // Left to right
    errorCurRow[x + 2] += (error * 7) >> 4;   // Right: 7/16
    errorNextRow[x] += (error * 3) >> 4;      // Bottom-left: 3/16
    errorNextRow[x + 1] += (error * 5) >> 4;  // Bottom: 5/16
    errorNextRow[x + 2] += (error) >> 4;      // Bottom-right: 1/16
  } else {
    // Right to left (mirrored)
    errorCurRow[x] += (error * 7) >> 4;       // Left: 7/16
    errorNextRow[x + 2] += (error * 3) >> 4;  // Bottom-right: 3/16
    errorNextRow[x + 1] += (error * 5) >> 4;  // Bottom: 5/16
    errorNextRow[x] += (error) >> 4;          // Bottom-left: 1/16
  }

  return quantized;
}

Bitmap::~Bitmap() {
  delete[] errorCurRow;
  delete[] errorNextRow;
}

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
    case BmpReaderError::ImageTooLarge:
      return "ImageTooLarge (max 2048x3072)";
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

  // Safety limits to prevent memory issues on ESP32
  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;
  if (width > MAX_IMAGE_WIDTH || height > MAX_IMAGE_HEIGHT) {
    return BmpReaderError::ImageTooLarge;
  }

  // Pre-calculate Row Bytes to avoid doing this every row
  rowBytes = (width * bpp + 31) / 32 * 4;

  for (int i = 0; i < 256; i++) paletteLum[i] = static_cast<uint8_t>(i);
  if (colorsUsed > 0) {
    for (uint32_t i = 0; i < colorsUsed; i++) {
      uint8_t rgb[4];
      file.read(rgb, 4);  // Read B, G, R, Reserved in one go
      paletteLum[i] = (77u * rgb[2] + 150u * rgb[1] + 29u * rgb[0]) >> 8;
    }
  }

  if (!file.seek(bfOffBits)) {
    return BmpReaderError::SeekPixelDataFailed;
  }

  // Allocate Floyd-Steinberg error buffers if enabled
  if (USE_FLOYD_STEINBERG) {
    delete[] errorCurRow;
    delete[] errorNextRow;
    errorCurRow = new int16_t[width + 2]();  // +2 for boundary handling
    errorNextRow = new int16_t[width + 2]();
    lastRowY = -1;
  }

  return BmpReaderError::Ok;
}

// packed 2bpp output, 0 = black, 1 = dark gray, 2 = light gray, 3 = white
BmpReaderError Bitmap::readRow(uint8_t* data, uint8_t* rowBuffer, int rowY) const {
  // Note: rowBuffer should be pre-allocated by the caller to size 'rowBytes'
  if (file.read(rowBuffer, rowBytes) != rowBytes) return BmpReaderError::ShortReadRow;

  // Handle Floyd-Steinberg error buffer progression
  const bool useFS = USE_FLOYD_STEINBERG && errorCurRow && errorNextRow;
  if (useFS) {
    // Check if we need to advance to next row (or reset if jumping)
    if (rowY != lastRowY + 1 && rowY != 0) {
      // Non-sequential row access - reset error buffers
      memset(errorCurRow, 0, (width + 2) * sizeof(int16_t));
      memset(errorNextRow, 0, (width + 2) * sizeof(int16_t));
    } else if (rowY > 0) {
      // Sequential access - swap buffers
      int16_t* temp = errorCurRow;
      errorCurRow = errorNextRow;
      errorNextRow = temp;
      memset(errorNextRow, 0, (width + 2) * sizeof(int16_t));
    }
    lastRowY = rowY;
  }

  uint8_t* outPtr = data;
  uint8_t currentOutByte = 0;
  int bitShift = 6;
  int currentX = 0;

  // Helper lambda to pack 2bpp color into the output stream
  auto packPixel = [&](const uint8_t lum) {
    uint8_t color;
    if (useFS) {
      // Floyd-Steinberg error diffusion
      color = quantizeFloydSteinberg(lum, currentX, width, errorCurRow, errorNextRow, false);
    } else {
      // Simple quantization or noise dithering
      color = quantize(lum, currentX, rowY);
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

  uint8_t lum;

  switch (bpp) {
    case 32: {
      const uint8_t* p = rowBuffer;
      for (int x = 0; x < width; x++) {
        lum = (77u * p[2] + 150u * p[1] + 29u * p[0]) >> 8;
        packPixel(lum);
        p += 4;
      }
      break;
    }
    case 24: {
      const uint8_t* p = rowBuffer;
      for (int x = 0; x < width; x++) {
        lum = (77u * p[2] + 150u * p[1] + 29u * p[0]) >> 8;
        packPixel(lum);
        p += 3;
      }
      break;
    }
    case 8: {
      for (int x = 0; x < width; x++) {
        packPixel(paletteLum[rowBuffer[x]]);
      }
      break;
    }
    case 2: {
      for (int x = 0; x < width; x++) {
        lum = paletteLum[(rowBuffer[x >> 2] >> (6 - ((x & 3) * 2))) & 0x03];
        packPixel(lum);
      }
      break;
    }
    case 1: {
      for (int x = 0; x < width; x++) {
        lum = (rowBuffer[x >> 3] & (0x80 >> (x & 7))) ? 0xFF : 0x00;
        packPixel(lum);
      }
      break;
    }
    default:
      return BmpReaderError::UnsupportedBpp;
  }

  // Flush remaining bits if width is not a multiple of 4
  if (bitShift != 6) *outPtr = currentOutByte;

  return BmpReaderError::Ok;
}

BmpReaderError Bitmap::rewindToData() const {
  if (!file.seek(bfOffBits)) {
    return BmpReaderError::SeekPixelDataFailed;
  }

  // Reset Floyd-Steinberg error buffers when rewinding
  if (USE_FLOYD_STEINBERG && errorCurRow && errorNextRow) {
    memset(errorCurRow, 0, (width + 2) * sizeof(int16_t));
    memset(errorNextRow, 0, (width + 2) * sizeof(int16_t));
    lastRowY = -1;
  }

  return BmpReaderError::Ok;
}
