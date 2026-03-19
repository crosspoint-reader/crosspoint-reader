#pragma once

#include <Memory.h>

#include <cstdint>
#include <cstring>

struct BmpHeader;

// Helper functions
uint8_t quantize(int gray, int x, int y);
uint8_t quantizeSimple(int gray);
uint8_t quantize1bit(int gray, int x, int y);
int adjustPixel(int gray);

// Populates a 1-bit BMP header in the provided memory.
void createBmpHeader(BmpHeader* bmpHeader, int width, int height);

// 1-bit Atkinson dithering - better quality than noise dithering for thumbnails
// Error distribution pattern (same as 2-bit but quantizes to 2 levels):
//     X  1/8 1/8
// 1/8 1/8 1/8
//     1/8
class Atkinson1BitDitherer {
 public:
  explicit Atkinson1BitDitherer(int width)
      : width(width),
        errorRow0(makeUniqueNoThrow<int16_t[]>(width + 4)),
        errorRow1(makeUniqueNoThrow<int16_t[]>(width + 4)),
        errorRow2(makeUniqueNoThrow<int16_t[]>(width + 4)) {}

  Atkinson1BitDitherer(const Atkinson1BitDitherer&) = delete;
  Atkinson1BitDitherer& operator=(const Atkinson1BitDitherer&) = delete;

  bool isValid() const { return errorRow0 && errorRow1 && errorRow2; }

  uint8_t processPixel(int gray, int x) {
    // Apply brightness/contrast/gamma adjustments
    gray = adjustPixel(gray);

    // Add accumulated error
    int adjusted = gray + errorRow0[x + 2];
    if (adjusted < 0) adjusted = 0;
    if (adjusted > 255) adjusted = 255;

    // Quantize to 2 levels (1-bit): 0 = black, 1 = white
    uint8_t quantized;
    int quantizedValue;
    if (adjusted < 128) {
      quantized = 0;
      quantizedValue = 0;
    } else {
      quantized = 1;
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
    std::swap(errorRow0, errorRow1);
    std::swap(errorRow1, errorRow2);
    memset(errorRow2.get(), 0, (width + 4) * sizeof(int16_t));
  }

  void reset() {
    memset(errorRow0.get(), 0, (width + 4) * sizeof(int16_t));
    memset(errorRow1.get(), 0, (width + 4) * sizeof(int16_t));
    memset(errorRow2.get(), 0, (width + 4) * sizeof(int16_t));
  }

 private:
  int width;
  std::unique_ptr<int16_t[]> errorRow0;
  std::unique_ptr<int16_t[]> errorRow1;
  std::unique_ptr<int16_t[]> errorRow2;
};

// Atkinson dithering - distributes only 6/8 (75%) of error for cleaner results
// Error distribution pattern:
//     X  1/8 1/8
// 1/8 1/8 1/8
//     1/8
// Less error buildup = fewer artifacts than Floyd-Steinberg
class AtkinsonDitherer {
 public:
  explicit AtkinsonDitherer(int width)
      : width(width),
        errorRow0(makeUniqueNoThrow<int16_t[]>(width + 4)),
        errorRow1(makeUniqueNoThrow<int16_t[]>(width + 4)),
        errorRow2(makeUniqueNoThrow<int16_t[]>(width + 4)) {}

  AtkinsonDitherer(const AtkinsonDitherer&) = delete;
  AtkinsonDitherer& operator=(const AtkinsonDitherer&) = delete;

  bool isValid() const { return errorRow0 && errorRow1 && errorRow2; }

  uint8_t processPixel(int gray, int x) {
    // Add accumulated error
    int adjusted = gray + errorRow0[x + 2];
    if (adjusted < 0) adjusted = 0;
    if (adjusted > 255) adjusted = 255;

    // Quantize to 4 levels
    uint8_t quantized;
    int quantizedValue;
    if (false) {  // original thresholds
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
    } else {  // fine-tuned to X4 eink display
      if (adjusted < 30) {
        quantized = 0;
        quantizedValue = 15;
      } else if (adjusted < 50) {
        quantized = 1;
        quantizedValue = 30;
      } else if (adjusted < 140) {
        quantized = 2;
        quantizedValue = 80;
      } else {
        quantized = 3;
        quantizedValue = 210;
      }
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
    std::swap(errorRow0, errorRow1);
    std::swap(errorRow1, errorRow2);
    memset(errorRow2.get(), 0, (width + 4) * sizeof(int16_t));
  }

  void reset() {
    memset(errorRow0.get(), 0, (width + 4) * sizeof(int16_t));
    memset(errorRow1.get(), 0, (width + 4) * sizeof(int16_t));
    memset(errorRow2.get(), 0, (width + 4) * sizeof(int16_t));
  }

 private:
  int width;
  std::unique_ptr<int16_t[]> errorRow0;
  std::unique_ptr<int16_t[]> errorRow1;
  std::unique_ptr<int16_t[]> errorRow2;
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
  explicit FloydSteinbergDitherer(int width)
      : width(width),
        errorCurRow(makeUniqueNoThrow<int16_t[]>(width + 2)),
        errorNextRow(makeUniqueNoThrow<int16_t[]>(width + 2)) {}

  FloydSteinbergDitherer(const FloydSteinbergDitherer&) = delete;
  FloydSteinbergDitherer& operator=(const FloydSteinbergDitherer&) = delete;

  bool isValid() const { return errorCurRow && errorNextRow; }

  // Process a single pixel and return quantized 2-bit value
  // x is the logical x position (0 to width-1), direction handled internally
  uint8_t processPixel(int gray, int x) {
    // Add accumulated error to this pixel
    int adjusted = gray + errorCurRow[x + 1];

    // Clamp to valid range
    if (adjusted < 0) adjusted = 0;
    if (adjusted > 255) adjusted = 255;

    // Quantize to 4 levels (0, 85, 170, 255)
    uint8_t quantized;
    int quantizedValue;
    if (false) {  // original thresholds
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
    } else {  // fine-tuned to X4 eink display
      if (adjusted < 30) {
        quantized = 0;
        quantizedValue = 15;
      } else if (adjusted < 50) {
        quantized = 1;
        quantizedValue = 30;
      } else if (adjusted < 140) {
        quantized = 2;
        quantizedValue = 80;
      } else {
        quantized = 3;
        quantizedValue = 210;
      }
    }

    // Calculate error
    int error = adjusted - quantizedValue;

    // Distribute error to neighbors (serpentine: direction-aware)
    if (!isReverseRow()) {
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
    std::swap(errorCurRow, errorNextRow);
    memset(errorNextRow.get(), 0, (width + 2) * sizeof(int16_t));
    rowCount++;
  }

  // Check if current row should be processed in reverse
  bool isReverseRow() const { return (rowCount & 1) != 0; }

  // Reset for a new image or MCU block
  void reset() {
    memset(errorCurRow.get(), 0, (width + 2) * sizeof(int16_t));
    memset(errorNextRow.get(), 0, (width + 2) * sizeof(int16_t));
    rowCount = 0;
  }

 private:
  int width;
  int rowCount{0};
  std::unique_ptr<int16_t[]> errorCurRow;
  std::unique_ptr<int16_t[]> errorNextRow;
};
