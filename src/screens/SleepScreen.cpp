#include "SleepScreen.h"

#include <GfxRenderer.h>
#include <SD.h>
#include <Arduino.h>

#include "CrossPointSettings.h"
#include "config.h"
#include "images/CrossLarge.h"

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
uint8_t* loadBMP(const char* filename, int& width, int& height) {
  // Using rotation type 1: 90 degrees clockwise
  const int rotationType = 1;
  Serial.printf("[SleepScreen] Trying to load BMP: %s\n", filename);

  if (!SD.exists(filename)) {
    Serial.printf("[SleepScreen] File not found: %s\n", filename);
    return nullptr;
  }

  File bmpFile = SD.open(filename);
  if (!bmpFile) {
    Serial.printf("[SleepScreen] Failed to open file: %s\n", filename);
    return nullptr;
  }

  // Read BMP header
  BMPHeader header;
  bmpFile.read((uint8_t*)&header, sizeof(BMPHeader));

  // Check if this is a valid BMP file
  if (header.signature != 0x4D42) { // "BM" in little-endian
    Serial.println("[SleepScreen] Invalid BMP signature");
    bmpFile.close();
    return nullptr;
  }

  // Check for supported bit depths
  if (header.bitsPerPixel != 1 && header.bitsPerPixel != 24) {
    Serial.printf("[SleepScreen] Unsupported bit depth: %d\n", header.bitsPerPixel);
    bmpFile.close();
    return nullptr;
  }

  // Get image dimensions
  width = header.width;
  height = (header.height < 0) ? -header.height : header.height; // Handle top-down BMPs
  bool topDown = (header.height < 0);

  Serial.printf("[SleepScreen] BMP dimensions: %dx%d, %d bits/pixel\n", width, height, header.bitsPerPixel);

  // Calculate destination dimensions based on rotation type
  int destWidth, destHeight;

  if (rotationType == 0 || rotationType == 2) {
    // No rotation or 180 degree rotation: keep same dimensions
    destWidth = width;
    destHeight = height;
  } else {
    // 90 degree rotations (clockwise or counterclockwise): swap width and height
    destWidth = height;
    destHeight = width;
  }

  Serial.printf("[SleepScreen] Output dimensions: %dx%d (rotation type: %d)\n",
                destWidth, destHeight, rotationType);

  // E-ink display: 1 bit per pixel (8 pixels per byte), MSB first format
  int bytesPerRow = (destWidth + 7) / 8;  // Round up to nearest byte
  int bufferSize = bytesPerRow * destHeight;

  Serial.printf("[SleepScreen] Buffer dimensions: %d bytes per row, %d total bytes\n", bytesPerRow, bufferSize);

  Serial.printf("[SleepScreen] Buffer size: %d bytes (%d bytes per row)\n", bufferSize, bytesPerRow);

  // Allocate memory for the display image
  uint8_t* displayImage = (uint8_t*)malloc(bufferSize);
  if (!displayImage) {
    Serial.println("[SleepScreen] Failed to allocate memory for display image");
    bmpFile.close();
    return nullptr;
  }

  // Initialize to all white (0xFF = all bits set to 1)
  memset(displayImage, 0xFF, bufferSize);

  // Calculate BMP file row size (padded to 4-byte boundaries)
  int bmpRowSize;
  if (header.bitsPerPixel == 1) {
    bmpRowSize = ((width + 31) / 32) * 4; // 1 bit per pixel, 4-byte alignment
  } else { // 24-bit
    bmpRowSize = ((width * 3 + 3) / 4) * 4; // 3 bytes per pixel (RGB), 4-byte alignment
  }

  // Allocate buffer for reading BMP rows
  uint8_t* rowBuffer = (uint8_t*)malloc(bmpRowSize);
  if (!rowBuffer) {
    Serial.println("[SleepScreen] Failed to allocate row buffer");
    free(displayImage);
    bmpFile.close();
    return nullptr;
  }

  // Process each row
  for (int y = 0; y < height; y++) {
    // Calculate source row (BMPs are normally stored bottom-to-top)
    int bmpRow = topDown ? y : (height - 1 - y);

    // Read one row from the BMP file
    bmpFile.seek(header.dataOffset + (bmpRow * bmpRowSize));
    bmpFile.read(rowBuffer, bmpRowSize);

    // Process each pixel in the row
    for (int x = 0; x < width; x++) {
      // Determine if this pixel should be black based on bit depth
      bool isBlack;

      if (header.bitsPerPixel == 1) {
        // For 1-bit BMPs, read the bit directly
        int byteIndex = x / 8;
        int bitIndex = 7 - (x % 8); // MSB first in BMP file format

        // In 1-bit BMPs, bit value 1 typically means black and 0 means white
        // Check if the bit is set (1) at the specified position
        isBlack = (rowBuffer[byteIndex] & (1 << bitIndex)) != 0;
      } else { // 24-bit
        // For 24-bit BMPs, convert RGB to grayscale
        // BMP stores colors as BGR (Blue, Green, Red)
        int byteIndex = x * 3;
        uint8_t blue = rowBuffer[byteIndex];
        uint8_t green = rowBuffer[byteIndex + 1];
        uint8_t red = rowBuffer[byteIndex + 2];

        // Convert to grayscale using standard luminance formula
        uint8_t gray = (red * 30 + green * 59 + blue * 11) / 100;

        // If below threshold (128), consider it black
        isBlack = (gray < 128);
      }

      // Apply 90 degree clockwise rotation
      // For rotation type 1: destX = y, destY = width - 1 - x
      int destX = y;
      int destY = width - 1 - x;

      // Calculate byte and bit position (1 bit per pixel)
      int destByteIndex = destY * bytesPerRow + (destX / 8);
      int destBitIndex = 7 - (destX % 8);  // MSB first (leftmost pixel in highest bit)

      // For e-ink display: 0=black, 1=white
      if (isBlack) {
        // Set to black (0) by clearing the corresponding bit
        displayImage[destByteIndex] &= ~(1 << destBitIndex);
      }
      // White pixels (1) are already set to 1 by the memset(0xFF) initialization
    }
  }

  // Clean up
  free(rowBuffer);
  bmpFile.close();

  Serial.printf("[SleepScreen] Successfully loaded BMP: %dx%d\n", width, height);
  return displayImage;
}

void SleepScreen::onEnter() {
  const auto pageWidth = GfxRenderer::getScreenWidth();
  const auto pageHeight = GfxRenderer::getScreenHeight();

  Serial.printf("[SleepScreen] Screen dimensions: %dx%d\n", pageWidth, pageHeight);

  renderer.clearScreen();

  // Try to load custom sleep image
  int imageWidth = 0;
  int imageHeight = 0;
  uint8_t* imageData = nullptr;

  // Try different possible paths
  const char* bmpPaths[] = {"sleep.bmp", "/sleep.bmp", "/SD/sleep.bmp"};

  // Try loading from different paths
  for (const char* path : bmpPaths) {
    imageData = loadBMP(path, imageWidth, imageHeight);
    if (imageData) {
        Serial.printf("[SleepScreen] Successfully loaded: %s\n", path);
        break;
    }
  }

  if (imageData) {
    // Image loaded successfully
    Serial.printf("[SleepScreen] Drawing image: %dx%d\n", imageWidth, imageHeight);
    Serial.printf("[SleepScreen] Screen dimensions: %dx%d\n", pageWidth, pageHeight);

    // Print the first 16 bytes of image data (for debugging)
    Serial.print("[SleepScreen] Image data sample: ");
    for (int i = 0; i < 16 && i < (imageWidth+7)/8 * imageHeight; i++) {
      Serial.printf("%02X ", imageData[i]);
    }
    Serial.println();

    // Calculate position to center the image
    int xPos = (pageWidth - imageWidth) / 2;
    int yPos = (pageHeight - imageHeight) / 2;
    if (xPos < 0) xPos = 0;
    if (yPos < 0) yPos = 0;

    // Draw the image - this sends the bitmap data to the e-ink display
    // Note: We've applied 90-degree clockwise rotation to compensate for
    // the renderer's behavior and ensure the image appears correctly
    // on the e-ink display.
    Serial.printf("[SleepScreen] Drawing at position: %d,%d (dimensions: %dx%d)\n",
                 xPos, yPos, imageWidth, imageHeight);
    renderer.drawImage(imageData, xPos, yPos, imageWidth, imageHeight);

    // Free the image data
    free(imageData);
  } else {
    // Fall back to default image
    Serial.println("[SleepScreen] Failed to load sleep.bmp - using default image");
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
