#include "SleepScreen.h"

#include <GfxRenderer.h>
#include <SD.h>
#include <Arduino.h>
#include <cstdint>

#include "CrossPointSettings.h"
#include "config.h"
#include "images/CrossLarge.h"

void convertBmpToBWdisplayData(uint8_t* bmpData,
    int width, int height, uint8_t* displayImage){
    // Convert BMP data to 1bpp format for e-ink display
    // Implementation details depend on the specific BMP format and display requirements

    // Static lookup table for bit masks (better performance)
    static const uint8_t bitMasks[8] = {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01};

    // For 1bpp images where width is divisible by 8, we can use a highly optimized approach
    const int bytesPerSrcRow = width / 8;
    const int bytesPerRow = (height + 7) / 8;  // use heigth due to 90deg rotation

    int bmpRowSize = width / 8; // 1 bit per pixel, assuming width is divisible by 8

    // Process each source row
    for (int y = 0; y < height; y++) {
      // Calculate source row (BMPs are normally stored bottom-to-top)
      int srcRow = height-1-y;

      // In 90-degree rotation, source Y becomes destination X
      int destX = y;
      int destByteX = destX / 8;
      int destBitInByte = destX & 0x07; // Fast mod 8
      uint8_t destBitMask = bitMasks[destBitInByte];

      // Get pointer to this row's data
      uint8_t* srcRowData = bmpData + (srcRow * bmpRowSize);

      // Process all bytes in this row
      for (int xByte = 0; xByte < bytesPerSrcRow; xByte++) {
        uint8_t srcByte = srcRowData[xByte];

        // Skip processing if byte is all white
        if (srcByte == 0xFF) continue;

        // For bytes that are either all black or have a simple pattern, optimize
        if (srcByte == 0x00) {
          // All 8 pixels are black - use fast path
          for (int bit = 0; bit < 8; bit++) {
            int srcX = (xByte * 8) + bit;
            int destY = width - 1 - srcX;
            int destByteIdx = (destY * bytesPerRow) + destByteX;
            displayImage[destByteIdx] &= ~destBitMask;
          }
        } else {
          // Process individual bits for mixed bytes
          for (int bit = 0; bit < 8; bit++) {
            // Only process if this bit is black (0)
            if ((srcByte & bitMasks[bit]) == 0) {
              int srcX = (xByte * 8) + bit;
              int destY = width - 1 - srcX;
              int destByteIdx = (destY * bytesPerRow) + destByteX;
              displayImage[destByteIdx] &= ~destBitMask;
            }
          }
        }
      }
    }
}
// BMP file header structure
#pragma pack(push, 1)
struct BMPHeader {
  uint16_t signature;      // 'BM'
  uint32_t fileSize;       // Size of the BMP file in bytes
  uint32_t reserved;       // Reserved
  uint32_t dataOffset;     // Offset to bitmap data
  uint32_t headerSize;     // Size of the header
  int32_t width;           // Width of the image
  int32_t height;          // Height of the image
  uint16_t planes;         // Number of color planes
  uint16_t bitsPerPixel;   // Bits per pixel
  uint32_t compression;    // Compression method
  uint32_t imageSize;      // Size of the image data
  int32_t xPixelsPerMeter; // Horizontal resolution
  int32_t yPixelsPerMeter; // Vertical resolution
  uint32_t totalColors;    // Number of colors in palette
  uint32_t importantColors;// Number of important colors
};
#pragma pack(pop)

// Load BMP file from SD card and rotate 90 degrees clockwise
// This rotation matches what we need for the e-ink display
// Returns a buffer with data formatted for the e-ink display (1bpp, MSB first)
uint8_t* loadBMP(const char* filename, int& width, int& height) {
  const unsigned long startTime = millis();
  Serial.printf("[%lu] [SleepScreen] Trying to load BMP: %s\n", millis(), filename);

  if (!SD.exists(filename)) {
    Serial.printf("[%lu] [SleepScreen] File not found: %s\n", millis(), filename);
    return nullptr;
  }

  File bmpFile = SD.open(filename);
  if (!bmpFile) {
    Serial.printf("[%lu] [SleepScreen] Failed to open file: %s\n", millis(), filename);
    return nullptr;
  }

  // Read BMP header
  BMPHeader header;
  bmpFile.read((uint8_t*)&header, sizeof(BMPHeader));

  // Check if this is a valid BMP file
  if (header.signature != 0x4D42) { // "BM" in little-endian
    Serial.printf("[%lu] [SleepScreen] Invalid BMP signature\n", millis());
    bmpFile.close();
    return nullptr;
  }

  // Check for supported bit depths
  if (header.bitsPerPixel != 1){ //} && header.bitsPerPixel != 24) {
    Serial.printf("[%lu] [SleepScreen] Unsupported bit depth: %d\n", millis(), header.bitsPerPixel);
    bmpFile.close();
    return nullptr;
  }
  if (header.height < 0){ //} && header.bitsPerPixel != 24) {
    Serial.printf("[%lu] [SleepScreen] Unsupported negative height\n", millis());
    bmpFile.close();
    return nullptr;
  }

  // Get image dimensions
  width = header.width;
  height = header.height;

  Serial.printf("[%lu] [SleepScreen] BMP dimensions: %dx%d, %d bits/pixel\n", millis(), width, height, header.bitsPerPixel);

  // Calculate destination dimensions based on rotation type
  int destWidth, destHeight;

  // 90 degree rotation to match display orientation
  destWidth = height;
  destHeight = width;

  // E-ink display: 1 bit per pixel (8 pixels per byte), MSB first format
  int bytesPerRow = (destWidth + 7) / 8;  // Round up to nearest byte
  int bufferSize = bytesPerRow * destHeight;

  // Allocate memory for the display image
  uint8_t* displayImage = (uint8_t*)malloc(bufferSize);
  if (!displayImage) {
    Serial.printf("[%lu] [SleepScreen] Failed to allocate memory for display image\n", millis());
    bmpFile.close();
    return nullptr;
  }

  // Initialize to all white (0xFF = all bits set to 1)
  memset(displayImage, 0xFF, bufferSize);


  // With 4-byte divisibility assertion, no padding calculations are needed

  // Add assertion that dimensions are divisible by 4
  if (width % 4 != 0 || height % 4 != 0) {
    Serial.printf("[%lu] [SleepScreen] Image dimensions not divisible by 4: %dx%d\n", millis(), width, height);
    // Continue anyway - we're assuming divisibility
  }

  // Verify BMP width is divisible by 8 for 1bpp images (for byte alignment)
  if (header.bitsPerPixel == 1 && width % 8 != 0) {
    Serial.printf("[%lu] [SleepScreen] Warning: 1bpp BMP width not divisible by 8: %d\n", millis(), width);
  }

  // Optimized direct handling for 1bpp BMPs
  if (header.bitsPerPixel == 1) {
    // Calculate total file size needed for reading the whole bitmap at once
    const int totalBitmapSize = width * height / 8;

    // Allocate a buffer for the entire bitmap
    uint8_t* bmpData = (uint8_t*)malloc(totalBitmapSize);
    if (!bmpData) {
      Serial.printf("[%lu] [SleepScreen] Failed to allocate bitmap buffer (%d bytes)\n",
                   millis(), totalBitmapSize);
      free(displayImage);
      bmpFile.close();
      return nullptr;
    }

    // Read the entire bitmap data at once (efficient bulk loading)
    bmpFile.seek(header.dataOffset);
    bmpFile.read(bmpData, totalBitmapSize);

    convertBmpToBWdisplayData(bmpData, width, height,displayImage);
    // Clean up
    free(bmpData);
  }
  else {
    // Handle 24-bit BMPs with bulk loading approach for better performance
    const int totalBitmapSize = 3 * width * height;
    uint8_t* bmpData = (uint8_t*)malloc(totalBitmapSize);

    // Calculate BMP file row size (with dimensions divisible by 4 assumption)
    int bmpRowSize = width * 3; // 3 bytes per pixel (RGB), no padding needed with width divisible by 4

    if (!bmpData) {
      Serial.printf("[%lu] [SleepScreen] Failed to allocate bitmap buffer\n", millis());
      free(displayImage);
      bmpFile.close();
      return nullptr;
    }

    // Read the entire bitmap data at once
    bmpFile.seek(header.dataOffset);
    bmpFile.read(bmpData, totalBitmapSize);

    // Static lookup table for bit masks
    static const uint8_t bitMasks[8] = {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01};

    // For color images, optimize with batch processing
    // Process in chunks of rows to improve cache locality
    const int CHUNK_SIZE = 8; // Process 8 rows at a time

    for (int chunkY = 0; chunkY < height; chunkY += CHUNK_SIZE) {
      const int rowsInChunk = min(CHUNK_SIZE, height - chunkY);

      // Process a chunk of rows
      for (int yOffset = 0; yOffset < rowsInChunk; yOffset++) {
        int y = chunkY + yOffset;

        // Calculate source row (BMPs are normally stored bottom-to-top)
        int bmpRow = height - 1 - y;

        // In 90-degree rotation, source Y becomes destination X
        int destX = y;
        int destByteX = destX / 8;
        int destBitInByte = destX & 0x07; // Fast mod 8
        uint8_t destBitMask = bitMasks[destBitInByte];

        // Get pointer to this row in the bitmap data
        uint8_t* rowData = bmpData + (bmpRow * bmpRowSize);

        // Process pixels in batches of 4 (since we know width is divisible by 4)
        for (int x = 0; x < width; x++) {
          // For 24-bit BMPs, convert RGB to grayscale
          int byteIndex = x * 3;

          // Fast grayscale approximation - R*0.299 + G*0.587 + B*0.114
          // Using bit-shifts for faster integer math: (r*76 + g*150 + b*30) >> 8
          uint8_t blue = rowData[byteIndex];
          uint8_t green = rowData[byteIndex + 1];
          uint8_t red = rowData[byteIndex + 2];

          // This is faster than division and gives nearly identical results
          uint16_t gray = ((red * 76) + (green * 150) + (blue * 30)) >> 8;

          // Skip white pixels
          if (gray >= 128) continue;

          // Apply 90 degree clockwise rotation: (x,y) -> (y, width-1-x)
          int destY = width - 1 - x;
          int destByteIdx = destY * bytesPerRow + destByteX;

          // Set to black
          displayImage[destByteIdx] &= ~destBitMask;
        }
      }
    }

    // Clean up
    free(bmpData);
  }
  bmpFile.close();

  const unsigned long elapsedTime = millis() - startTime;
  Serial.printf("[%lu] [SleepScreen] Successfully loaded BMP: %dx%d in %lu ms\n",
      millis(), destWidth, destHeight, elapsedTime);
  return displayImage;
}

void SleepScreen::onEnter() {
  const auto pageWidth = GfxRenderer::getScreenWidth();
  const auto pageHeight = GfxRenderer::getScreenHeight();

  renderer.clearScreen();

  // Try to load custom sleep image
  int imageWidth = 0;
  int imageHeight = 0;
  uint8_t* imageData = nullptr;

  // Try different possible paths
  const char* bmpPaths[] = {"/sleep24bpp.bmp", "/sleep.bmp"};

  // Try loading from different paths
  for (const char* path : bmpPaths) {
    imageData = loadBMP(path, imageWidth, imageHeight);
    if (imageData) {
        Serial.printf("[%lu] [SleepScreen] Successfully loaded: %s\n", millis(), path);
        break;
    }
  }

  if (imageData) {
    // Image loaded successfully
    Serial.printf("[%lu] [SleepScreen] Drawing image: %dx%d\n", millis(), imageWidth, imageHeight);

    // Calculate position to center the image
    int xPos = (pageWidth - imageWidth) / 2;
    int yPos = (pageHeight - imageHeight) / 2;
    if (xPos < 0) xPos = 0;
    if (yPos < 0) yPos = 0;

    // Draw the image - this sends the bitmap data to the e-ink display
    // Note: We've applied 90-degree clockwise rotation to compensate for
    // the renderer's behavior and ensure the image appears correctly
    // on the e-ink display.
    Serial.printf("[%lu] [SleepScreen] Drawing at position: %d,%d (dimensions: %dx%d)\n", millis(),
                 xPos, yPos, imageWidth, imageHeight);
    renderer.drawImage(imageData, xPos, yPos, imageWidth, imageHeight);

    // Free the image data
    free(imageData);
  } else {
    // Fall back to default image
    Serial.printf("[%lu] [SleepScreen] Failed to load sleep.bmp - using default image\n", millis());
    renderer.drawImage(CrossLarge, (pageWidth - 128) / 2, (pageHeight - 128) / 2, 128, 128);
    renderer.drawCenteredText(UI_FONT_ID, pageHeight / 2 + 70, "CrossPoint", true, BOLD);
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, "SLEEPING");
  }

  // Apply white screen if enabled in settings
  if (!SETTINGS.whiteSleepScreen) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
}
