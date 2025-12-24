#include "JpegToBmpConverter.h"

#include <picojpeg.h>

#include <cstdio>
#include <cstring>

// Context structure for picojpeg callback
struct JpegReadContext {
  File& file;
  uint8_t buffer[512];
  size_t bufferPos;
  size_t bufferFilled;
};

// ============================================================================
// IMAGE PROCESSING OPTIONS - Toggle these to test different configurations
// ============================================================================
constexpr bool USE_8BIT_OUTPUT = false;  // true: 8-bit grayscale (no quantization), false: 2-bit (4 levels)
// Dithering method selection (only one should be true, or all false for simple quantization):
constexpr bool USE_ATKINSON = true;          // Atkinson dithering (cleaner than F-S, less error diffusion)
constexpr bool USE_FLOYD_STEINBERG = false;  // Floyd-Steinberg error diffusion (can cause "worm" artifacts)
constexpr bool USE_NOISE_DITHERING = false;  // Hash-based noise dithering (good for downsampling)
// Brightness/Contrast adjustments:
constexpr bool USE_BRIGHTNESS = true;     // true: apply brightness/gamma adjustments
constexpr int BRIGHTNESS_BOOST = 10;      // Brightness offset (0-50)
constexpr bool GAMMA_CORRECTION = true;   // Gamma curve (brightens midtones)
constexpr float CONTRAST_FACTOR = 1.15f;  // Contrast multiplier (1.0 = no change, >1 = more contrast)
// Pre-resize to target display size (CRITICAL: avoids dithering artifacts from post-downsampling)
constexpr bool USE_PRESCALE = true;     // true: scale image to target size before dithering
constexpr int TARGET_MAX_WIDTH = 480;   // Max width for cover images (portrait display width)
constexpr int TARGET_MAX_HEIGHT = 800;  // Max height for cover images (portrait display height)
// ============================================================================

// Integer approximation of gamma correction (brightens midtones)
// Uses a simple curve: out = 255 * sqrt(in/255) ≈ sqrt(in * 255)
static inline int applyGamma(int gray) {
  if (!GAMMA_CORRECTION) return gray;
  // Fast integer square root approximation for gamma ~0.5 (brightening)
  // This brightens dark/mid tones while preserving highlights
  const int product = gray * 255;
  // Newton-Raphson integer sqrt (2 iterations for good accuracy)
  int x = gray;
  if (x > 0) {
    x = (x + product / x) >> 1;
    x = (x + product / x) >> 1;
  }
  return x > 255 ? 255 : x;
}

// Apply contrast adjustment around midpoint (128)
// factor > 1.0 increases contrast, < 1.0 decreases
static inline int applyContrast(int gray) {
  // Integer-based contrast: (gray - 128) * factor + 128
  // Using fixed-point: factor 1.15 ≈ 115/100
  constexpr int factorNum = static_cast<int>(CONTRAST_FACTOR * 100);
  int adjusted = ((gray - 128) * factorNum) / 100 + 128;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;
  return adjusted;
}

// Combined brightness/contrast/gamma adjustment
static inline int adjustPixel(int gray) {
  if (!USE_BRIGHTNESS) return gray;

  // Order: contrast first, then brightness, then gamma
  gray = applyContrast(gray);
  gray += BRIGHTNESS_BOOST;
  if (gray > 255) gray = 255;
  if (gray < 0) gray = 0;
  gray = applyGamma(gray);

  return gray;
}

// Simple quantization without dithering - just divide into 4 levels
static inline uint8_t quantizeSimple(int gray) {
  gray = adjustPixel(gray);
  // Simple 2-bit quantization: 0-63=0, 64-127=1, 128-191=2, 192-255=3
  return static_cast<uint8_t>(gray >> 6);
}

// Hash-based noise dithering - survives downsampling without moiré artifacts
// Uses integer hash to generate pseudo-random threshold per pixel
static inline uint8_t quantizeNoise(int gray, int x, int y) {
  gray = adjustPixel(gray);

  // Generate noise threshold using integer hash (no regular pattern to alias)
  uint32_t hash = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
  hash = (hash ^ (hash >> 13)) * 1274126177u;
  const int threshold = static_cast<int>(hash >> 24);  // 0-255

  // Map gray (0-255) to 4 levels with dithering
  const int scaled = gray * 3;

  if (scaled < 255) {
    return (scaled + threshold >= 255) ? 1 : 0;
  } else if (scaled < 510) {
    return ((scaled - 255) + threshold >= 255) ? 2 : 1;
  } else {
    return ((scaled - 510) + threshold >= 255) ? 3 : 2;
  }
}

// Main quantization function - selects between methods based on config
static inline uint8_t quantize(int gray, int x, int y) {
  if (USE_NOISE_DITHERING) {
    return quantizeNoise(gray, x, y);
  } else {
    return quantizeSimple(gray);
  }
}

// Atkinson dithering - distributes only 6/8 (75%) of error for cleaner results
// Error distribution pattern:
//     X  1/8 1/8
// 1/8 1/8 1/8
//     1/8
// Less error buildup = fewer artifacts than Floyd-Steinberg
class AtkinsonDitherer {
 public:
  AtkinsonDitherer(int width) : width(width) {
    errorRow0 = new int16_t[width + 4]();  // Current row
    errorRow1 = new int16_t[width + 4]();  // Next row
    errorRow2 = new int16_t[width + 4]();  // Row after next
  }

  ~AtkinsonDitherer() {
    delete[] errorRow0;
    delete[] errorRow1;
    delete[] errorRow2;
  }

  uint8_t processPixel(int gray, int x) {
    // Apply brightness/contrast/gamma adjustments
    gray = adjustPixel(gray);

    // Add accumulated error
    int adjusted = gray + errorRow0[x + 2];
    if (adjusted < 0) adjusted = 0;
    if (adjusted > 255) adjusted = 255;

    // Quantize to 4 levels
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

    // Calculate error (only distribute 6/8 = 75%)
    int error = (adjusted - quantizedValue) >> 3;  // error/8

    // Distribute 1/8 to each of 6 neighbors
    errorRow0[x + 3] += error;  // Right
    errorRow0[x + 4] += error;  // Right+1
    errorRow1[x + 1] += error;  // Bottom-left
    errorRow1[x + 2] += error;  // Bottom
    errorRow1[x + 3] += error;  // Bottom-right
    errorRow2[x + 2] += error;  // Two rows down

    return quantized;
  }

  void nextRow() {
    int16_t* temp = errorRow0;
    errorRow0 = errorRow1;
    errorRow1 = errorRow2;
    errorRow2 = temp;
    memset(errorRow2, 0, (width + 4) * sizeof(int16_t));
  }

  void reset() {
    memset(errorRow0, 0, (width + 4) * sizeof(int16_t));
    memset(errorRow1, 0, (width + 4) * sizeof(int16_t));
    memset(errorRow2, 0, (width + 4) * sizeof(int16_t));
  }

 private:
  int width;
  int16_t* errorRow0;
  int16_t* errorRow1;
  int16_t* errorRow2;
};

// Floyd-Steinberg error diffusion dithering with serpentine scanning
// Serpentine scanning alternates direction each row to reduce "worm" artifacts
// Error distribution pattern (left-to-right):
//       X   7/16
// 3/16 5/16 1/16
// Error distribution pattern (right-to-left, mirrored):
// 1/16 5/16 3/16
//      7/16  X
class FloydSteinbergDitherer {
 public:
  FloydSteinbergDitherer(int width) : width(width), rowCount(0) {
    errorCurRow = new int16_t[width + 2]();  // +2 for boundary handling
    errorNextRow = new int16_t[width + 2]();
  }

  ~FloydSteinbergDitherer() {
    delete[] errorCurRow;
    delete[] errorNextRow;
  }

  // Process a single pixel and return quantized 2-bit value
  // x is the logical x position (0 to width-1), direction handled internally
  uint8_t processPixel(int gray, int x, bool reverseDirection) {
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
    if (!reverseDirection) {
      // Left to right: standard distribution
      // Right: 7/16
      errorCurRow[x + 2] += (error * 7) >> 4;
      // Bottom-left: 3/16
      errorNextRow[x] += (error * 3) >> 4;
      // Bottom: 5/16
      errorNextRow[x + 1] += (error * 5) >> 4;
      // Bottom-right: 1/16
      errorNextRow[x + 2] += (error) >> 4;
    } else {
      // Right to left: mirrored distribution
      // Left: 7/16
      errorCurRow[x] += (error * 7) >> 4;
      // Bottom-right: 3/16
      errorNextRow[x + 2] += (error * 3) >> 4;
      // Bottom: 5/16
      errorNextRow[x + 1] += (error * 5) >> 4;
      // Bottom-left: 1/16
      errorNextRow[x] += (error) >> 4;
    }

    return quantized;
  }

  // Call at the end of each row to swap buffers
  void nextRow() {
    // Swap buffers
    int16_t* temp = errorCurRow;
    errorCurRow = errorNextRow;
    errorNextRow = temp;
    // Clear the next row buffer
    memset(errorNextRow, 0, (width + 2) * sizeof(int16_t));
    rowCount++;
  }

  // Check if current row should be processed in reverse
  bool isReverseRow() const { return (rowCount & 1) != 0; }

  // Reset for a new image or MCU block
  void reset() {
    memset(errorCurRow, 0, (width + 2) * sizeof(int16_t));
    memset(errorNextRow, 0, (width + 2) * sizeof(int16_t));
    rowCount = 0;
  }

 private:
  int width;
  int rowCount;
  int16_t* errorCurRow;
  int16_t* errorNextRow;
};

inline void write16(Print& out, const uint16_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
}

inline void write32(Print& out, const uint32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

inline void write32Signed(Print& out, const int32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

// Helper function: Write BMP header with 8-bit grayscale (256 levels)
void writeBmpHeader8bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width + 3) / 4 * 4;  // 8 bits per pixel, padded
  const int imageSize = bytesPerRow * height;
  const uint32_t paletteSize = 256 * 4;  // 256 colors * 4 bytes (BGRA)
  const uint32_t fileSize = 14 + 40 + paletteSize + imageSize;

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);                      // Reserved
  write32(bmpOut, 14 + 40 + paletteSize);  // Offset to pixel data

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 8);              // Bits per pixel (8 bits)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 256);   // colorsUsed
  write32(bmpOut, 256);   // colorsImportant

  // Color Palette (256 grayscale entries x 4 bytes = 1024 bytes)
  for (int i = 0; i < 256; i++) {
    bmpOut.write(static_cast<uint8_t>(i));  // Blue
    bmpOut.write(static_cast<uint8_t>(i));  // Green
    bmpOut.write(static_cast<uint8_t>(i));  // Red
    bmpOut.write(static_cast<uint8_t>(0));  // Reserved
  }
}

// Helper function: Write BMP header with 2-bit color depth
void JpegToBmpConverter::writeBmpHeader(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width * 2 + 31) / 32 * 4;  // 2 bits per pixel, round up
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 70 + imageSize;  // 14 (file header) + 40 (DIB header) + 16 (palette) + image

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);  // File size
  write32(bmpOut, 0);         // Reserved
  write32(bmpOut, 70);        // Offset to pixel data

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 2);              // Bits per pixel (2 bits)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 4);     // colorsUsed
  write32(bmpOut, 4);     // colorsImportant

  // Color Palette (4 colors x 4 bytes = 16 bytes)
  // Format: Blue, Green, Red, Reserved (BGRA)
  uint8_t palette[16] = {
      0x00, 0x00, 0x00, 0x00,  // Color 0: Black
      0x55, 0x55, 0x55, 0x00,  // Color 1: Dark gray (85)
      0xAA, 0xAA, 0xAA, 0x00,  // Color 2: Light gray (170)
      0xFF, 0xFF, 0xFF, 0x00   // Color 3: White
  };
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

// Callback function for picojpeg to read JPEG data
unsigned char JpegToBmpConverter::jpegReadCallback(unsigned char* pBuf, const unsigned char buf_size,
                                                   unsigned char* pBytes_actually_read, void* pCallback_data) {
  auto* context = static_cast<JpegReadContext*>(pCallback_data);

  if (!context || !context->file) {
    return PJPG_STREAM_READ_ERROR;
  }

  // Check if we need to refill our context buffer
  if (context->bufferPos >= context->bufferFilled) {
    context->bufferFilled = context->file.read(context->buffer, sizeof(context->buffer));
    context->bufferPos = 0;

    if (context->bufferFilled == 0) {
      // EOF or error
      *pBytes_actually_read = 0;
      return 0;  // Success (EOF is normal)
    }
  }

  // Copy available bytes to picojpeg's buffer
  const size_t available = context->bufferFilled - context->bufferPos;
  const size_t toRead = available < buf_size ? available : buf_size;

  memcpy(pBuf, context->buffer + context->bufferPos, toRead);
  context->bufferPos += toRead;
  *pBytes_actually_read = static_cast<unsigned char>(toRead);

  return 0;  // Success
}

// Core function: Convert JPEG file to 2-bit BMP
bool JpegToBmpConverter::jpegFileToBmpStream(File& jpegFile, Print& bmpOut) {
  Serial.printf("[%lu] [JPG] Converting JPEG to BMP\n", millis());

  // Setup context for picojpeg callback
  JpegReadContext context = {.file = jpegFile, .bufferPos = 0, .bufferFilled = 0};

  // Initialize picojpeg decoder
  pjpeg_image_info_t imageInfo;
  const unsigned char status = pjpeg_decode_init(&imageInfo, jpegReadCallback, &context, 0);
  if (status != 0) {
    Serial.printf("[%lu] [JPG] JPEG decode init failed with error code: %d\n", millis(), status);
    return false;
  }

  Serial.printf("[%lu] [JPG] JPEG dimensions: %dx%d, components: %d, MCUs: %dx%d\n", millis(), imageInfo.m_width,
                imageInfo.m_height, imageInfo.m_comps, imageInfo.m_MCUSPerRow, imageInfo.m_MCUSPerCol);

  // Safety limits to prevent memory issues on ESP32
  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;
  constexpr int MAX_MCU_ROW_BYTES = 65536;

  if (imageInfo.m_width > MAX_IMAGE_WIDTH || imageInfo.m_height > MAX_IMAGE_HEIGHT) {
    Serial.printf("[%lu] [JPG] Image too large (%dx%d), max supported: %dx%d\n", millis(), imageInfo.m_width,
                  imageInfo.m_height, MAX_IMAGE_WIDTH, MAX_IMAGE_HEIGHT);
    return false;
  }

  // Calculate output dimensions (pre-scale to fit display exactly)
  int outWidth = imageInfo.m_width;
  int outHeight = imageInfo.m_height;
  // Use fixed-point scaling (16.16) for sub-pixel accuracy
  uint32_t scaleX_fp = 65536;  // 1.0 in 16.16 fixed point
  uint32_t scaleY_fp = 65536;
  bool needsScaling = false;

  if (USE_PRESCALE && (imageInfo.m_width > TARGET_MAX_WIDTH || imageInfo.m_height > TARGET_MAX_HEIGHT)) {
    // Calculate scale to fit within target dimensions while maintaining aspect ratio
    const float scaleToFitWidth = static_cast<float>(TARGET_MAX_WIDTH) / imageInfo.m_width;
    const float scaleToFitHeight = static_cast<float>(TARGET_MAX_HEIGHT) / imageInfo.m_height;
    const float scale = (scaleToFitWidth < scaleToFitHeight) ? scaleToFitWidth : scaleToFitHeight;

    outWidth = static_cast<int>(imageInfo.m_width * scale);
    outHeight = static_cast<int>(imageInfo.m_height * scale);

    // Ensure at least 1 pixel
    if (outWidth < 1) outWidth = 1;
    if (outHeight < 1) outHeight = 1;

    // Calculate fixed-point scale factors (source pixels per output pixel)
    // scaleX_fp = (srcWidth << 16) / outWidth
    scaleX_fp = (static_cast<uint32_t>(imageInfo.m_width) << 16) / outWidth;
    scaleY_fp = (static_cast<uint32_t>(imageInfo.m_height) << 16) / outHeight;
    needsScaling = true;

    Serial.printf("[%lu] [JPG] Pre-scaling %dx%d -> %dx%d (fit to %dx%d)\n", millis(), imageInfo.m_width,
                  imageInfo.m_height, outWidth, outHeight, TARGET_MAX_WIDTH, TARGET_MAX_HEIGHT);
  }

  // Write BMP header with output dimensions
  int bytesPerRow;
  if (USE_8BIT_OUTPUT) {
    writeBmpHeader8bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth + 3) / 4 * 4;
  } else {
    writeBmpHeader(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth * 2 + 31) / 32 * 4;
  }

  // Allocate row buffer
  auto* rowBuffer = static_cast<uint8_t*>(malloc(bytesPerRow));
  if (!rowBuffer) {
    Serial.printf("[%lu] [JPG] Failed to allocate row buffer\n", millis());
    return false;
  }

  // Allocate a buffer for one MCU row worth of grayscale pixels
  // This is the minimal memory needed for streaming conversion
  const int mcuPixelHeight = imageInfo.m_MCUHeight;
  const int mcuRowPixels = imageInfo.m_width * mcuPixelHeight;

  // Validate MCU row buffer size before allocation
  if (mcuRowPixels > MAX_MCU_ROW_BYTES) {
    Serial.printf("[%lu] [JPG] MCU row buffer too large (%d bytes), max: %d\n", millis(), mcuRowPixels,
                  MAX_MCU_ROW_BYTES);
    free(rowBuffer);
    return false;
  }

  auto* mcuRowBuffer = static_cast<uint8_t*>(malloc(mcuRowPixels));
  if (!mcuRowBuffer) {
    Serial.printf("[%lu] [JPG] Failed to allocate MCU row buffer (%d bytes)\n", millis(), mcuRowPixels);
    free(rowBuffer);
    return false;
  }

  // Create ditherer if enabled (only for 2-bit output)
  // Use OUTPUT dimensions for dithering (after prescaling)
  AtkinsonDitherer* atkinsonDitherer = nullptr;
  FloydSteinbergDitherer* fsDitherer = nullptr;
  if (!USE_8BIT_OUTPUT) {
    if (USE_ATKINSON) {
      atkinsonDitherer = new AtkinsonDitherer(outWidth);
    } else if (USE_FLOYD_STEINBERG) {
      fsDitherer = new FloydSteinbergDitherer(outWidth);
    }
  }

  // For scaling: accumulate source rows into scaled output rows
  // We need to track which source Y maps to which output Y
  // Using fixed-point: srcY_fp = outY * scaleY_fp (gives source Y in 16.16 format)
  uint32_t* rowAccum = nullptr;    // Accumulator for each output X (32-bit for larger sums)
  uint16_t* rowCount = nullptr;    // Count of source pixels accumulated per output X
  int currentOutY = 0;             // Current output row being accumulated
  uint32_t nextOutY_srcStart = 0;  // Source Y where next output row starts (16.16 fixed point)

  if (needsScaling) {
    rowAccum = new uint32_t[outWidth]();
    rowCount = new uint16_t[outWidth]();
    nextOutY_srcStart = scaleY_fp;  // First boundary is at scaleY_fp (source Y for outY=1)
  }

  // Process MCUs row-by-row and write to BMP as we go (top-down)
  const int mcuPixelWidth = imageInfo.m_MCUWidth;

  for (int mcuY = 0; mcuY < imageInfo.m_MCUSPerCol; mcuY++) {
    // Clear the MCU row buffer
    memset(mcuRowBuffer, 0, mcuRowPixels);

    // Decode one row of MCUs
    for (int mcuX = 0; mcuX < imageInfo.m_MCUSPerRow; mcuX++) {
      const unsigned char mcuStatus = pjpeg_decode_mcu();
      if (mcuStatus != 0) {
        if (mcuStatus == PJPG_NO_MORE_BLOCKS) {
          Serial.printf("[%lu] [JPG] Unexpected end of blocks at MCU (%d, %d)\n", millis(), mcuX, mcuY);
        } else {
          Serial.printf("[%lu] [JPG] JPEG decode MCU failed at (%d, %d) with error code: %d\n", millis(), mcuX, mcuY,
                        mcuStatus);
        }
        free(mcuRowBuffer);
        free(rowBuffer);
        return false;
      }

      // picojpeg stores MCU data in 8x8 blocks
      // Block layout: H2V2(16x16)=0,64,128,192 H2V1(16x8)=0,64 H1V2(8x16)=0,128
      for (int blockY = 0; blockY < mcuPixelHeight; blockY++) {
        for (int blockX = 0; blockX < mcuPixelWidth; blockX++) {
          const int pixelX = mcuX * mcuPixelWidth + blockX;
          if (pixelX >= imageInfo.m_width) continue;

          // Calculate proper block offset for picojpeg buffer
          const int blockCol = blockX / 8;
          const int blockRow = blockY / 8;
          const int localX = blockX % 8;
          const int localY = blockY % 8;
          const int blocksPerRow = mcuPixelWidth / 8;
          const int blockIndex = blockRow * blocksPerRow + blockCol;
          const int pixelOffset = blockIndex * 64 + localY * 8 + localX;

          uint8_t gray;
          if (imageInfo.m_comps == 1) {
            gray = imageInfo.m_pMCUBufR[pixelOffset];
          } else {
            const uint8_t r = imageInfo.m_pMCUBufR[pixelOffset];
            const uint8_t g = imageInfo.m_pMCUBufG[pixelOffset];
            const uint8_t b = imageInfo.m_pMCUBufB[pixelOffset];
            gray = (r * 25 + g * 50 + b * 25) / 100;
          }

          mcuRowBuffer[blockY * imageInfo.m_width + pixelX] = gray;
        }
      }
    }

    // Process source rows from this MCU row
    const int startRow = mcuY * mcuPixelHeight;
    const int endRow = (mcuY + 1) * mcuPixelHeight;

    for (int y = startRow; y < endRow && y < imageInfo.m_height; y++) {
      const int bufferY = y - startRow;

      if (!needsScaling) {
        // No scaling - direct output (1:1 mapping)
        memset(rowBuffer, 0, bytesPerRow);

        if (USE_8BIT_OUTPUT) {
          for (int x = 0; x < outWidth; x++) {
            const uint8_t gray = mcuRowBuffer[bufferY * imageInfo.m_width + x];
            rowBuffer[x] = adjustPixel(gray);
          }
        } else {
          for (int x = 0; x < outWidth; x++) {
            const uint8_t gray = mcuRowBuffer[bufferY * imageInfo.m_width + x];
            uint8_t twoBit;
            if (atkinsonDitherer) {
              twoBit = atkinsonDitherer->processPixel(gray, x);
            } else if (fsDitherer) {
              twoBit = fsDitherer->processPixel(gray, x, fsDitherer->isReverseRow());
            } else {
              twoBit = quantize(gray, x, y);
            }
            const int byteIndex = (x * 2) / 8;
            const int bitOffset = 6 - ((x * 2) % 8);
            rowBuffer[byteIndex] |= (twoBit << bitOffset);
          }
          if (atkinsonDitherer)
            atkinsonDitherer->nextRow();
          else if (fsDitherer)
            fsDitherer->nextRow();
        }
        bmpOut.write(rowBuffer, bytesPerRow);
      } else {
        // Fixed-point area averaging for exact fit scaling
        // For each output pixel X, accumulate source pixels that map to it
        // srcX range for outX: [outX * scaleX_fp >> 16, (outX+1) * scaleX_fp >> 16)
        const uint8_t* srcRow = mcuRowBuffer + bufferY * imageInfo.m_width;

        for (int outX = 0; outX < outWidth; outX++) {
          // Calculate source X range for this output pixel
          const int srcXStart = (static_cast<uint32_t>(outX) * scaleX_fp) >> 16;
          const int srcXEnd = (static_cast<uint32_t>(outX + 1) * scaleX_fp) >> 16;

          // Accumulate all source pixels in this range
          int sum = 0;
          int count = 0;
          for (int srcX = srcXStart; srcX < srcXEnd && srcX < imageInfo.m_width; srcX++) {
            sum += srcRow[srcX];
            count++;
          }

          // Handle edge case: if no pixels in range, use nearest
          if (count == 0 && srcXStart < imageInfo.m_width) {
            sum = srcRow[srcXStart];
            count = 1;
          }

          rowAccum[outX] += sum;
          rowCount[outX] += count;
        }

        // Check if we've crossed into the next output row
        // Current source Y in fixed point: y << 16
        const uint32_t srcY_fp = static_cast<uint32_t>(y + 1) << 16;

        // Output row when source Y crosses the boundary
        if (srcY_fp >= nextOutY_srcStart && currentOutY < outHeight) {
          memset(rowBuffer, 0, bytesPerRow);

          if (USE_8BIT_OUTPUT) {
            for (int x = 0; x < outWidth; x++) {
              const uint8_t gray = (rowCount[x] > 0) ? (rowAccum[x] / rowCount[x]) : 0;
              rowBuffer[x] = adjustPixel(gray);
            }
          } else {
            for (int x = 0; x < outWidth; x++) {
              const uint8_t gray = (rowCount[x] > 0) ? (rowAccum[x] / rowCount[x]) : 0;
              uint8_t twoBit;
              if (atkinsonDitherer) {
                twoBit = atkinsonDitherer->processPixel(gray, x);
              } else if (fsDitherer) {
                twoBit = fsDitherer->processPixel(gray, x, fsDitherer->isReverseRow());
              } else {
                twoBit = quantize(gray, x, currentOutY);
              }
              const int byteIndex = (x * 2) / 8;
              const int bitOffset = 6 - ((x * 2) % 8);
              rowBuffer[byteIndex] |= (twoBit << bitOffset);
            }
            if (atkinsonDitherer)
              atkinsonDitherer->nextRow();
            else if (fsDitherer)
              fsDitherer->nextRow();
          }

          bmpOut.write(rowBuffer, bytesPerRow);
          currentOutY++;

          // Reset accumulators for next output row
          memset(rowAccum, 0, outWidth * sizeof(uint32_t));
          memset(rowCount, 0, outWidth * sizeof(uint16_t));

          // Update boundary for next output row
          nextOutY_srcStart = static_cast<uint32_t>(currentOutY + 1) * scaleY_fp;
        }
      }
    }
  }

  // Clean up
  if (rowAccum) {
    delete[] rowAccum;
  }
  if (rowCount) {
    delete[] rowCount;
  }
  if (atkinsonDitherer) {
    delete atkinsonDitherer;
  }
  if (fsDitherer) {
    delete fsDitherer;
  }
  free(mcuRowBuffer);
  free(rowBuffer);

  Serial.printf("[%lu] [JPG] Successfully converted JPEG to BMP\n", millis());
  return true;
}
