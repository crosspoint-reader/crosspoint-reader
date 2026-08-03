#include "Bitmap.h"

#include <Logging.h>
#include <Memory.h>

#include <cstdlib>
#include <cstring>

// ============================================================================
// IMAGE PROCESSING OPTIONS
// ============================================================================
// Dithering is applied when converting high-color BMPs to the display's native
// 2-bit (4-level) grayscale. Images whose palette entries all map to native
// gray levels (0, 85, 170, 255 ±21) are mapped directly without dithering.
// For cover images, dithering is done in JpegToBmpConverter.cpp instead.
constexpr bool USE_ATKINSON = true;  // Use Atkinson dithering instead of Floyd-Steinberg
constexpr uint32_t ADAPTIVE_TONE_LOW_PERCENTILE_PERMILLE = 10;
constexpr uint32_t ADAPTIVE_TONE_HIGH_PERCENTILE_PERMILLE = 990;
constexpr int ADAPTIVE_TONE_MIN_RANGE = 96;
constexpr int ADAPTIVE_TONE_BLEND_NUM = 3;
constexpr int ADAPTIVE_TONE_BLEND_DEN = 4;
// ============================================================================

Bitmap::~Bitmap() {
  delete[] errorCurRow;
  delete[] errorNextRow;

  delete atkinsonDitherer;
  delete fsDitherer;
}

uint16_t Bitmap::readLE16(HalFile& f) {
  const int c0 = f.read();
  const int c1 = f.read();
  const auto b0 = static_cast<uint8_t>(c0 < 0 ? 0 : c0);
  const auto b1 = static_cast<uint8_t>(c1 < 0 ? 0 : c1);
  return static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8);
}

uint32_t Bitmap::readLE32(HalFile& f) {
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
      return "UnsupportedBpp (expected 1, 2, 4, 8, 24, or 32)";
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

  file.seekCur(8);
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
  const bool validBpp = bpp == 1 || bpp == 2 || bpp == 4 || bpp == 8 || bpp == 24 || bpp == 32;

  if (planes != 1) return BmpReaderError::BadPlanes;
  if (!validBpp) return BmpReaderError::UnsupportedBpp;
  // Allow BI_RGB (0) for all, and BI_BITFIELDS (3) for 32bpp which is common for BGRA masks.
  if (!(comp == 0 || (bpp == 32 && comp == 3))) return BmpReaderError::UnsupportedCompression;

  file.seekCur(12);  // biSizeImage, biXPelsPerMeter, biYPelsPerMeter
  colorsUsed = readLE32(file);
  // BMP spec: colorsUsed==0 means default (2^bpp for paletted formats)
  if (colorsUsed == 0 && bpp <= 8) colorsUsed = 1u << bpp;
  if (colorsUsed > 256u) return BmpReaderError::PaletteTooLarge;
  file.seekCur(4);  // biClrImportant

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

  // Check if palette luminances map cleanly to the display's 4 native gray levels.
  // Native levels are 0, 85, 170, 255 — i.e. values where (lum >> 6) is lossless.
  // If all palette entries are near a native level, we can skip dithering entirely.
  nativePalette = bpp <= 2;  // 1-bit and 2-bit are always native
  if (!nativePalette && colorsUsed > 0) {
    nativePalette = true;
    for (uint32_t i = 0; i < colorsUsed; i++) {
      const uint8_t lum = paletteLum[i];
      const uint8_t level = lum >> 6;            // quantize to 0-3
      const uint8_t reconstructed = level * 85;  // back to 0, 85, 170, 255
      if (lum > reconstructed + 21 || lum + 21 < reconstructed) {
        nativePalette = false;  // luminance is too far from any native level
        break;
      }
    }
  }

  // Decide pixel processing strategy:
  //  - Native palette → direct mapping, no processing needed
  //  - High-color + dithering enabled → error-diffusion dithering (Atkinson or Floyd-Steinberg)
  //  - High-color + dithering disabled → simple quantization (no error diffusion)
  if (toneMapping == BitmapToneMapping::Adaptive && bpp > 1) {
    analyzeAdaptiveToneMapping();
    const BmpReaderError rewindResult = rewindToData();
    if (rewindResult != BmpReaderError::Ok) {
      LOG_ERR("BMP", "Failed to rewind after adaptive tone analysis");
      return rewindResult;
    }
  }

  const bool highColor = !nativePalette;
  if (highColor && dithering) {
    const Gray4QuantizationMode quantizationMode =
        adaptiveToneMapping ? Gray4QuantizationMode::Native : Gray4QuantizationMode::DisplayTuned;
    if (USE_ATKINSON) {
      atkinsonDitherer = new AtkinsonDitherer(width, quantizationMode);
    } else {
      fsDitherer = new FloydSteinbergDitherer(width, quantizationMode);
    }
  }

  return BmpReaderError::Ok;
}

uint8_t Bitmap::applyAdaptiveTone(const uint8_t luminance) const {
  if (!adaptiveToneMapping || adaptiveWhitePoint <= adaptiveBlackPoint) {
    return luminance;
  }

  int leveled;
  if (luminance <= adaptiveBlackPoint) {
    leveled = 0;
  } else if (luminance >= adaptiveWhitePoint) {
    leveled = 255;
  } else {
    leveled = ((static_cast<int>(luminance) - adaptiveBlackPoint) * 255) /
              (static_cast<int>(adaptiveWhitePoint) - adaptiveBlackPoint);
  }

  int adjusted = (static_cast<int>(luminance) * (ADAPTIVE_TONE_BLEND_DEN - ADAPTIVE_TONE_BLEND_NUM) +
                  leveled * ADAPTIVE_TONE_BLEND_NUM) /
                 ADAPTIVE_TONE_BLEND_DEN;
  if (luminance > 242 && adjusted < luminance) {
    adjusted = luminance;
  }
  if (adjusted < 0) return 0;
  if (adjusted > 255) return 255;
  return static_cast<uint8_t>(adjusted);
}

bool Bitmap::analyzeAdaptiveToneMapping() {
  adaptiveToneMapping = false;
  adaptiveBlackPoint = 0;
  adaptiveWhitePoint = 255;

  auto histogram = makeUniqueNoThrow<uint32_t[]>(256);
  if (!histogram) {
    LOG_ERR("BMP", "Failed to allocate adaptive tone histogram (%u bytes)",
            static_cast<unsigned>(256u * sizeof(uint32_t)));
    return false;
  }

  // A BMP scanline can be up to 8192 bytes at the parser's 2048 px width limit.
  // Heap storage keeps it off the small task stack and is released after this one-shot analysis.
  auto rowBuffer = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(rowBytes));
  if (!rowBuffer) {
    LOG_ERR("BMP", "Failed to allocate adaptive tone row buffer (%d bytes)", rowBytes);
    return false;
  }

  if (!file.seek(bfOffBits)) {
    LOG_ERR("BMP", "Failed to seek for adaptive tone analysis");
    return false;
  }

  uint64_t total = 0;
  for (int y = 0; y < height; y++) {
    if (file.read(rowBuffer.get(), rowBytes) != rowBytes) {
      LOG_ERR("BMP", "Short read during adaptive tone analysis at row %d", y);
      return false;
    }

    switch (bpp) {
      case 32: {
        const uint8_t* pixel = rowBuffer.get();
        for (int x = 0; x < width; x++) {
          const uint8_t luminance = (77u * pixel[2] + 150u * pixel[1] + 29u * pixel[0]) >> 8;
          histogram[luminance]++;
          pixel += 4;
        }
        break;
      }
      case 24: {
        const uint8_t* pixel = rowBuffer.get();
        for (int x = 0; x < width; x++) {
          const uint8_t luminance = (77u * pixel[2] + 150u * pixel[1] + 29u * pixel[0]) >> 8;
          histogram[luminance]++;
          pixel += 3;
        }
        break;
      }
      case 8:
        for (int x = 0; x < width; x++) {
          histogram[paletteLum[rowBuffer[x]]]++;
        }
        break;
      case 4:
        for (int x = 0; x < width; x++) {
          const uint8_t index = (x & 1) ? (rowBuffer[x >> 1] & 0x0F) : (rowBuffer[x >> 1] >> 4);
          histogram[paletteLum[index]]++;
        }
        break;
      case 2:
        for (int x = 0; x < width; x++) {
          const uint8_t index = (rowBuffer[x >> 2] >> (6 - ((x & 3) * 2))) & 0x03;
          histogram[paletteLum[index]]++;
        }
        break;
      case 1:
        for (int x = 0; x < width; x++) {
          const uint8_t index = (rowBuffer[x >> 3] & (0x80 >> (x & 7))) ? 1 : 0;
          histogram[paletteLum[index]]++;
        }
        break;
      default:
        LOG_ERR("BMP", "Unsupported bpp during adaptive tone analysis: %u", static_cast<unsigned>(bpp));
        return false;
    }
    total += static_cast<uint32_t>(width);
  }

  const uint64_t lowTarget = (total * ADAPTIVE_TONE_LOW_PERCENTILE_PERMILLE) / 1000u;
  const uint64_t highTarget = (total * ADAPTIVE_TONE_HIGH_PERCENTILE_PERMILLE) / 1000u;
  uint64_t cumulative = 0;
  uint8_t low = 0;
  uint8_t high = 255;
  bool foundLow = false;
  for (int i = 0; i < 256; i++) {
    cumulative += histogram[i];
    if (!foundLow && cumulative >= lowTarget) {
      low = static_cast<uint8_t>(i);
      foundLow = true;
    }
    if (cumulative >= highTarget) {
      high = static_cast<uint8_t>(i);
      break;
    }
  }

  if (static_cast<int>(high) - static_cast<int>(low) < ADAPTIVE_TONE_MIN_RANGE) {
    LOG_DBG("BMP", "Adaptive tone skipped: black=%u white=%u", static_cast<unsigned>(low), static_cast<unsigned>(high));
    return false;
  }

  adaptiveBlackPoint = low;
  adaptiveWhitePoint = high;
  adaptiveToneMapping = true;
  LOG_DBG("BMP", "Adaptive tone enabled: black=%u white=%u", static_cast<unsigned>(adaptiveBlackPoint),
          static_cast<unsigned>(adaptiveWhitePoint));
  return true;
}

// packed 2bpp output, 0 = black, 1 = dark gray, 2 = light gray, 3 = white
BmpReaderError Bitmap::readNextRow(uint8_t* data, uint8_t* rowBuffer) const {
  // Note: rowBuffer should be pre-allocated by the caller to size 'rowBytes'
  if (file.read(rowBuffer, rowBytes) != rowBytes) return BmpReaderError::ShortReadRow;

  prevRowY += 1;

  uint8_t* outPtr = data;
  uint8_t currentOutByte = 0;
  int bitShift = 6;
  int currentX = 0;

  // Helper lambda to pack 2bpp color into the output stream
  auto packPixel = [&](const uint8_t lum) {
    const uint8_t toneMappedLum = applyAdaptiveTone(lum);
    uint8_t color;
    if (atkinsonDitherer) {
      color = atkinsonDitherer->processPixel(adjustPixel(toneMappedLum), currentX);
    } else if (fsDitherer) {
      color = fsDitherer->processPixel(adjustPixel(toneMappedLum), currentX);
    } else {
      const int adjusted = adjustPixel(toneMappedLum);
      if (nativePalette) {
        // Palette matches native gray levels: direct mapping (still apply brightness/contrast/gamma)
        color = static_cast<uint8_t>(adjusted >> 6);
      } else {
        // Non-native palette with dithering disabled: simple quantization
        color = adaptiveToneMapping ? quantizeGray4(adjusted, Gray4QuantizationMode::Native).index
                                    : quantize(adjusted, currentX, prevRowY);
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
    case 4: {
      for (int x = 0; x < width; x++) {
        const uint8_t nibble = (x & 1) ? (rowBuffer[x >> 1] & 0x0F) : (rowBuffer[x >> 1] >> 4);
        packPixel(paletteLum[nibble]);
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
        // Get palette index (0 or 1) from bit at position x
        const uint8_t palIndex = (rowBuffer[x >> 3] & (0x80 >> (x & 7))) ? 1 : 0;
        // Use palette lookup for proper black/white mapping
        lum = paletteLum[palIndex];
        packPixel(lum);
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

  // Flush remaining bits if width is not a multiple of 4
  if (bitShift != 6) *outPtr = currentOutByte;

  return BmpReaderError::Ok;
}

BmpReaderError Bitmap::rewindToData() const {
  if (!file.seek(bfOffBits)) {
    return BmpReaderError::SeekPixelDataFailed;
  }

  // Reset dithering when rewinding
  if (fsDitherer) fsDitherer->reset();
  if (atkinsonDitherer) atkinsonDitherer->reset();

  return BmpReaderError::Ok;
}
