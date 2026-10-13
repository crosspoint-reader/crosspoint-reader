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

// Adaptive tone mapping constants and the shared percentile/mapping logic live in
// AdaptiveTone.h, which the PNG framebuffer decoder also uses. See that header for
// the attribution and for why the two decoders feed it differently.
// ============================================================================

Bitmap::~Bitmap() {
  delete[] errorCurRow;
  delete[] errorNextRow;

  delete atkinsonDitherer;
  delete fsDitherer;
}

uint16_t Bitmap::readLE16(FsFile& f) {
  const int c0 = f.read();
  const int c1 = f.read();
  const auto b0 = static_cast<uint8_t>(c0 < 0 ? 0 : c0);
  const auto b1 = static_cast<uint8_t>(c1 < 0 ? 0 : c1);
  return static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8);
}

uint32_t Bitmap::readLE32(FsFile& f) {
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
    // The palette follows the whole DIB header, not the 40 bytes read above: GIMP and ImageMagick
    // write V4/V5 headers (108/124 bytes), whose masks and colour-space fields were read as colours,
    // which turned a 1-bit image's black and white into two greys. A palette that claims more
    // entries than fit before the pixels is cut to the ones that do.
    // Ported from CrossInk commits 7d49b69fa ("read colors correctly in extended BMP files") and
    // 99712ef66 ("reject incomplete BMP color palettes"), Julia Nguyen; clamping instead of
    // rejecting the overlapping palette is ours.
    const uint64_t paletteStart = 14ULL + biSize;
    const uint64_t room = bfOffBits > paletteStart ? (bfOffBits - paletteStart) / 4 : 0;
    if (colorsUsed > room) colorsUsed = static_cast<uint32_t>(room);
    if (!file.seek(static_cast<size_t>(paletteStart))) return BmpReaderError::SeekPixelDataFailed;
    for (uint32_t i = 0; i < colorsUsed; i++) {
      uint8_t rgb[4];
      if (file.read(rgb, 4) != 4) return BmpReaderError::FileInvalid;  // B, G, R, reserved
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
  // Adaptive analysis needs its own full read of the pixel data, so it has to run
  // here (before any row is decoded) and rewind afterwards. Requires at least 8 bpp:
  // that gives 256 distinct luminances, enough for percentiles to mean something.
  // Below that (<=4 bpp, so <=16 levels, and only 4 at 2 bpp) they do not, and such
  // images take the nativePalette path anyway.
  if (toneMapping != BitmapToneMapping::None && bpp >= 8) {
    analyzeAdaptiveTone();
    const BmpReaderError rewindResult = rewindToData();
    if (rewindResult != BmpReaderError::Ok) {
      LOG_ERR("BMP", "Failed to rewind after adaptive tone analysis");
      return rewindResult;
    }
  }

  const bool highColor = !nativePalette;
  if (highColor && dithering) {
    // Always DisplayTuned, tone mapping or not: the tone curve corrects the IMAGE's
    // range and the quantizer's thresholds correct for the PANEL, so the two are
    // orthogonal and there is nothing for a mode swap to avoid. The earlier swap to
    // Native on tone-mapped input was a real bug (near-black sleep covers), but so was
    // the thing it was compensating for -- DisplayTuned used to feed error diffusion a
    // bunched 15/30/80/210 level spacing, which is what actually washed the midtones
    // out. Both live under Gray4QuantizationMode now; see the note there.
    if (USE_ATKINSON) {
      atkinsonDitherer = new (std::nothrow) AtkinsonDitherer(width);
    } else {
      fsDitherer = new (std::nothrow) FloydSteinbergDitherer(width);
    }
  }

  return BmpReaderError::Ok;
}

void Bitmap::oneBitExpansion(uint16_t table[256]) const {
  // readNextRow()'s packPixel() for a native palette with no ditherer, which is every 1-bit image.
  uint8_t color[2];
  for (int index = 0; index < 2; ++index) {
    const int adjusted = adjustPixel(applyAdaptiveTone(paletteLum[index]));
    color[index] = static_cast<uint8_t>(adjusted >> 6);
  }
  for (int b = 0; b < 256; ++b) {
    uint8_t high = 0;
    uint8_t low = 0;
    for (int bit = 0; bit < 8; ++bit) {
      const uint8_t c = color[(b >> (7 - bit)) & 1];
      if (bit < 4) {
        high |= static_cast<uint8_t>(c << (6 - 2 * bit));
      } else {
        low |= static_cast<uint8_t>(c << (6 - 2 * (bit - 4)));
      }
    }
    table[b] = static_cast<uint16_t>((high << 8) | low);
  }
}

BmpReaderError Bitmap::readRawRows(uint8_t* buffer, const int rows) const {
  const size_t bytes = static_cast<size_t>(rows) * static_cast<size_t>(rowBytes);
  if (rows <= 0) return BmpReaderError::Ok;
  if (file.read(buffer, bytes) != static_cast<int>(bytes)) return BmpReaderError::ShortReadRow;
  prevRowY += rows;
  return BmpReaderError::Ok;
}

void Bitmap::expandOneBitRow(const uint16_t table[256], const uint8_t* raw, uint8_t* out, const int width) {
  const int fullBytes = width / 8;
  for (int i = 0; i < fullBytes; ++i) {
    const uint16_t pair = table[raw[i]];
    out[2 * i] = static_cast<uint8_t>(pair >> 8);
    out[2 * i + 1] = static_cast<uint8_t>(pair);
  }
  const int rest = width % 8;
  if (rest == 0) return;
  // The first `rest` pixels of the last byte; the padding after them stays 0.
  const auto keep = static_cast<uint16_t>(0xFFFFu << (16 - 2 * rest));
  const uint16_t pair = table[raw[fullBytes]] & keep;
  out[2 * fullBytes] = static_cast<uint8_t>(pair >> 8);
  if (rest > 4) out[2 * fullBytes + 1] = static_cast<uint8_t>(pair);
}

uint8_t Bitmap::applyAdaptiveTone(const uint8_t luminance) const {
  return adaptive_tone::apply(adaptiveTonePoints, luminance);
}

// Builds a 256-bin luminance histogram over a row-subsampled pass and derives the tone
// curve from it, in whichever mode the caller selected. Leaves the points inactive (i.e.
// the renderer unchanged) on any allocation failure, short read, or a histogram the
// analysis declines, so a failed analysis degrades to current behaviour rather than to a
// broken image. Only called for bpp >= 8.
bool Bitmap::analyzeAdaptiveTone() {
  adaptiveTonePoints = {};

  auto histogram = makeUniqueNoThrow<uint32_t[]>(256);
  if (!histogram) {
    LOG_ERR("BMP", "Failed to allocate adaptive tone histogram");
    return false;
  }

  // Up to 8192 bytes at the parser's 2048 px width limit -- too large for the task
  // stack, and freed as soon as this one-shot analysis returns.
  auto rowBuffer = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(rowBytes));
  if (!rowBuffer) {
    LOG_ERR("BMP", "Failed to allocate adaptive tone row buffer (%d bytes)", rowBytes);
    return false;
  }

  if (!file.seek(bfOffBits)) {
    LOG_ERR("BMP", "Failed to seek for adaptive tone analysis");
    return false;
  }

  const int bytesPerPixel = bpp / 8;
  uint32_t sampled = 0;
  for (int y = 0; y < height; y += adaptive_tone::ROW_STEP) {
    if (file.read(rowBuffer.get(), rowBytes) != rowBytes) {
      LOG_ERR("BMP", "Short read during adaptive tone analysis at row %d", y);
      return false;
    }

    if (bpp == 8) {
      // Paletted: the byte is an index, and paletteLum already holds its luminance.
      for (int x = 0; x < width; x++) {
        histogram[paletteLum[rowBuffer[x]]]++;
      }
    } else {
      const uint8_t* pixel = rowBuffer.get();
      for (int x = 0; x < width; x++) {
        histogram[(77u * pixel[2] + 150u * pixel[1] + 29u * pixel[0]) >> 8]++;
        pixel += bytesPerPixel;
      }
    }
    sampled += static_cast<uint32_t>(width);

    // Skip the rows we are not sampling rather than reading them.
    const int skipRows = (y + adaptive_tone::ROW_STEP < height) ? adaptive_tone::ROW_STEP - 1 : 0;
    if (skipRows > 0 && !file.seekCur(static_cast<int32_t>(skipRows) * rowBytes)) {
      LOG_ERR("BMP", "Failed to skip rows during adaptive tone analysis");
      return false;
    }
  }

  if (sampled == 0) return false;

  adaptiveTonePoints = adaptive_tone::derivePoints(histogram.get(), sampled, toneAnalysisMode(toneMapping), eqBlendNum);
  if (!adaptiveTonePoints.active) {
    LOG_DBG("BMP", "Adaptive tone (%s) declined: line art or range too narrow",
            toneMapping == BitmapToneMapping::Equalize ? "equalize" : "stretch");
    return false;
  }

  LOG_DBG("BMP", "Adaptive tone (%s) enabled: black=%u white=%u",
          toneMapping == BitmapToneMapping::Equalize ? "equalize" : "stretch",
          static_cast<unsigned>(adaptiveTonePoints.blackPoint), static_cast<unsigned>(adaptiveTonePoints.whitePoint));
  return true;
}

// packed 2bpp output, 0 = black, 1 = dark gray, 2 = light gray, 3 = white
BmpReaderError Bitmap::readNextRow(uint8_t* data, uint8_t* rowBuffer, uint8_t* gray8Row) const {
  // Note: rowBuffer should be pre-allocated by the caller to size 'rowBytes'
  if (file.read(rowBuffer, rowBytes) != rowBytes) return BmpReaderError::ShortReadRow;

  prevRowY += 1;

  uint8_t* outPtr = data;
  uint8_t currentOutByte = 0;
  int bitShift = 6;
  int currentX = 0;

  // Helper lambda to pack 2bpp color into the output stream
  auto packPixel = [&](const uint8_t rawLum) {
    const uint8_t lum = applyAdaptiveTone(rawLum);
    // Capture the sample BEFORE any quantiser sees it. Deliberately computed
    // alongside rather than shared with the branches below: they each reach
    // adjustPixel() by their own route, and rewiring that to hand one value to
    // both outputs would change the 2-bit rendition every existing panel shows.
    if (gray8Row) {
      const int adjusted = adjustPixel(lum);
      gray8Row[currentX] = static_cast<uint8_t>(adjusted < 0 ? 0 : (adjusted > 255 ? 255 : adjusted));
    }
    uint8_t color;
    if (atkinsonDitherer) {
      color = atkinsonDitherer->processPixel(adjustPixel(lum), currentX);
    } else if (fsDitherer) {
      color = fsDitherer->processPixel(adjustPixel(lum), currentX);
    } else {
      const int adjusted = adjustPixel(lum);
      if (nativePalette) {
        // Palette matches native gray levels: direct mapping (still apply brightness/contrast/gamma)
        color = static_cast<uint8_t>(adjusted >> 6);
      } else {
        // Non-native palette with dithering disabled: simple quantization. Tone-mapped
        // input takes this path too -- the curve has already corrected the image, and
        // these thresholds still have to account for the panel (see parseHeaders).
        color = quantize(adjusted, currentX, prevRowY);
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
