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

// Helper function: Convert 8-bit grayscale to 2-bit (0-3)
uint8_t JpegToBmpConverter::grayscaleTo2Bit(const uint8_t grayscale) {
  // Simple threshold mapping:
  // 0-63 -> 0 (black)
  // 64-127 -> 1 (dark gray)
  // 128-191 -> 2 (light gray)
  // 192-255 -> 3 (white)
  return grayscale >> 6;
}

inline void write16(Print& out, const uint16_t value) {
  // out.write(reinterpret_cast<const uint8_t *>(&value), 2);
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
}

inline void write32(Print& out, const uint32_t value) {
  // out.write(reinterpret_cast<const uint8_t *>(&value), 4);
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

inline void write32Signed(Print& out, const int32_t value) {
  // out.write(reinterpret_cast<const uint8_t *>(&value), 4);
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
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

  // Write BMP header
  writeBmpHeader(bmpOut, imageInfo.m_width, imageInfo.m_height);

  // Calculate row parameters
  const int bytesPerRow = (imageInfo.m_width * 2 + 31) / 32 * 4;

  // Allocate row buffer for packed 2-bit pixels
  auto* rowBuffer = static_cast<uint8_t*>(malloc(bytesPerRow));
  if (!rowBuffer) {
    Serial.printf("[%lu] [JPG] Failed to allocate row buffer\n", millis());
    return false;
  }

  // Allocate a buffer for one MCU row worth of grayscale pixels
  // This is the minimal memory needed for streaming conversion
  const int mcuPixelHeight = imageInfo.m_MCUHeight;
  const int mcuRowPixels = imageInfo.m_width * mcuPixelHeight;
  auto* mcuRowBuffer = static_cast<uint8_t*>(malloc(mcuRowPixels));
  if (!mcuRowBuffer) {
    Serial.printf("[%lu] [JPG] Failed to allocate MCU row buffer\n", millis());
    free(rowBuffer);
    return false;
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

      // Process MCU block into MCU row buffer
      // MCUs are composed of 8x8 blocks. For 16x16 MCUs, there are four 8x8 blocks:
      // Block layout for 16x16 MCU:  [0, 64]  (top row of blocks)
      //                              [128, 192] (bottom row of blocks)
      for (int blockY = 0; blockY < mcuPixelHeight; blockY++) {
        for (int blockX = 0; blockX < mcuPixelWidth; blockX++) {
          const int pixelX = mcuX * mcuPixelWidth + blockX;

          // Skip pixels outside image width (can happen with MCU alignment)
          if (pixelX >= imageInfo.m_width) {
            continue;
          }

          // Calculate which 8x8 block and position within that block
          const int block8x8Col = blockX / 8;  // 0 or 1 for 16-wide MCU
          const int block8x8Row = blockY / 8;  // 0 or 1 for 16-tall MCU
          const int pixelInBlockX = blockX % 8;
          const int pixelInBlockY = blockY % 8;

          // Calculate byte offset: each 8x8 block is 64 bytes
          // Blocks are arranged: [0, 64], [128, 192]
          const int blockOffset = (block8x8Row * (mcuPixelWidth / 8) + block8x8Col) * 64;
          const int mcuIndex = blockOffset + pixelInBlockY * 8 + pixelInBlockX;

          // Get grayscale value
          uint8_t gray;
          if (imageInfo.m_comps == 1) {
            // Grayscale image
            gray = imageInfo.m_pMCUBufR[mcuIndex];
          } else {
            // RGB image - convert to grayscale
            const uint8_t r = imageInfo.m_pMCUBufR[mcuIndex];
            const uint8_t g = imageInfo.m_pMCUBufG[mcuIndex];
            const uint8_t b = imageInfo.m_pMCUBufB[mcuIndex];
            // Luminance formula: Y = 0.299*R + 0.587*G + 0.114*B
            // Using integer approximation: (30*R + 59*G + 11*B) / 100
            gray = (r * 30 + g * 59 + b * 11) / 100;
          }

          // Store grayscale value in MCU row buffer
          mcuRowBuffer[blockY * imageInfo.m_width + pixelX] = gray;
        }
      }
    }

    // Write all pixel rows from this MCU row to BMP file
    const int startRow = mcuY * mcuPixelHeight;
    const int endRow = (mcuY + 1) * mcuPixelHeight;

    for (int y = startRow; y < endRow && y < imageInfo.m_height; y++) {
      memset(rowBuffer, 0, bytesPerRow);

      // Pack 4 pixels per byte (2 bits each)
      for (int x = 0; x < imageInfo.m_width; x++) {
        const int bufferY = y - startRow;
        const uint8_t gray = mcuRowBuffer[bufferY * imageInfo.m_width + x];
        const uint8_t twoBit = grayscaleTo2Bit(gray);

        const int byteIndex = (x * 2) / 8;
        const int bitOffset = 6 - ((x * 2) % 8);  // 6, 4, 2, 0
        rowBuffer[byteIndex] |= (twoBit << bitOffset);
      }

      // Write row with padding
      bmpOut.write(rowBuffer, bytesPerRow);
    }
  }

  // Clean up
  free(mcuRowBuffer);
  free(rowBuffer);

  Serial.printf("[%lu] [JPG] Successfully converted JPEG to BMP\n", millis());
  return true;
}
