#include "BmpStreamWriter.h"

#include <Logging.h>
#include <Memory.h>
#include <Print.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstring>

#include "BitmapHelpers.h"

namespace {

constexpr uint32_t FP_ONE = 1UL << 16;

void write16(Print& output, const uint16_t value) {
  output.write(value & 0xFF);
  output.write((value >> 8) & 0xFF);
}

void write32(Print& output, const uint32_t value) {
  output.write(value & 0xFF);
  output.write((value >> 8) & 0xFF);
  output.write((value >> 16) & 0xFF);
  output.write((value >> 24) & 0xFF);
}

void writeBmpHeader(Print& output, const int width, const int height, const int bitsPerPixel) {
  const int paletteColors = 1 << bitsPerPixel;
  const int bytesPerRow = (width * bitsPerPixel + 31) / 32 * 4;
  const uint32_t imageSize = bytesPerRow * height;
  const uint32_t pixelOffset = 14 + 40 + paletteColors * 4;

  output.write('B');
  output.write('M');
  write32(output, pixelOffset + imageSize);
  write32(output, 0);
  write32(output, pixelOffset);

  write32(output, 40);
  write32(output, static_cast<uint32_t>(width));
  write32(output, static_cast<uint32_t>(-height));
  write16(output, 1);
  write16(output, bitsPerPixel);
  write32(output, 0);
  write32(output, imageSize);
  write32(output, 2835);
  write32(output, 2835);
  write32(output, paletteColors);
  write32(output, paletteColors);

  for (int i = 0; i < paletteColors; i++) {
    const uint8_t gray = static_cast<uint8_t>(i * 255 / (paletteColors - 1));
    output.write(gray);
    output.write(gray);
    output.write(gray);
    output.write(static_cast<uint8_t>(0));
  }
}

}  // namespace

bool calculateBmpOutputSize(const int sourceWidth, const int sourceHeight, const int targetWidth,
                            const int targetHeight, const bool crop, int& outputWidth, int& outputHeight) {
  if (sourceWidth <= 0 || sourceHeight <= 0) return false;

  outputWidth = sourceWidth;
  outputHeight = sourceHeight;
  if (targetWidth <= 0 || targetHeight <= 0 || (sourceWidth == targetWidth && sourceHeight == targetHeight)) {
    return true;
  }

  const float scaleToFitWidth = static_cast<float>(targetWidth) / sourceWidth;
  const float scaleToFitHeight = static_cast<float>(targetHeight) / sourceHeight;
  const float scale = crop ? std::max(scaleToFitWidth, scaleToFitHeight) : std::min(scaleToFitWidth, scaleToFitHeight);
  outputWidth = std::max(1, static_cast<int>(sourceWidth * scale));
  outputHeight = std::max(1, static_cast<int>(sourceHeight * scale));
  return true;
}

BmpStreamWriter::BmpStreamWriter() = default;
BmpStreamWriter::~BmpStreamWriter() = default;

bool BmpStreamWriter::begin(Print& output, const int width, const int height, const bool oneBit,
                            const bool originalThresholds) {
  if (width <= 0 || height <= 0) return false;

  this->output = &output;
  this->width = width;
  this->height = height;
  this->oneBit = oneBit;
  bytesPerRow = (width * (oneBit ? 1 : 2) + 31) / 32 * 4;

  packedRow = makeUniqueNoThrow<uint8_t[]>(bytesPerRow);
  if (!packedRow) {
    LOG_ERR("BMP", "OOM: BMP row buffer (%d bytes)", bytesPerRow);
    return false;
  }

  if (oneBit) {
    atkinson1BitDitherer = makeUniqueNoThrow<Atkinson1BitDitherer>(width);
    if (!atkinson1BitDitherer || !atkinson1BitDitherer->isValid()) {
      LOG_ERR("BMP", "OOM: 1-bit ditherer");
      return false;
    }
  } else {
    atkinsonDitherer = makeUniqueNoThrow<AtkinsonDitherer>(width, originalThresholds);
    if (!atkinsonDitherer || !atkinsonDitherer->isValid()) {
      LOG_ERR("BMP", "OOM: 2-bit ditherer");
      return false;
    }
  }

  writeBmpHeader(output, width, height, oneBit ? 1 : 2);
  return true;
}

bool BmpStreamWriter::writeRow(const uint8_t* grayRow, const int y) {
  if (!output || !grayRow || y < 0 || y >= height) return false;
  memset(packedRow.get(), 0, bytesPerRow);

  if (oneBit) {
    for (int x = 0; x < width; x++) {
      packedRow[x / 8] |= atkinson1BitDitherer->processPixel(grayRow[x], x) << (7 - x % 8);
    }
    atkinson1BitDitherer->nextRow();
  } else {
    for (int x = 0; x < width; x++) {
      const uint8_t gray = adjustPixel(grayRow[x]);
      packedRow[(x * 2) / 8] |= atkinsonDitherer->processPixel(gray, x) << (6 - (x * 2) % 8);
    }
    atkinsonDitherer->nextRow();
  }

  if (output->write(packedRow.get(), bytesPerRow) != static_cast<size_t>(bytesPerRow)) {
    LOG_ERR("BMP", "Failed to write row %d", y);
    return false;
  }
  if (++rowsSinceYield >= 8) {
    rowsSinceYield = 0;
    vTaskDelay(1);
  }
  return true;
}

uint8_t* BmpStreamWriter::getScratchRow() {
  if (!scratchRow) {
    scratchRow = makeUniqueNoThrow<uint8_t[]>(width);
    if (!scratchRow) LOG_ERR("BMP", "OOM: scaled row buffer (%d bytes)", width);
  }
  return scratchRow.get();
}

bool GrayRowScaler::begin(const int sourceWidth, const int sourceHeight, BmpStreamWriter& output) {
  if (sourceWidth <= 0 || sourceHeight <= 0) return false;

  this->sourceWidth = sourceWidth;
  outputWidth = output.getWidth();
  outputHeight = output.getHeight();
  this->output = &output;

  if (sourceWidth == outputWidth && sourceHeight == outputHeight) return true;

  scaleX = (static_cast<uint32_t>(sourceWidth) << 16) / outputWidth;
  scaleY = (static_cast<uint32_t>(sourceHeight) << 16) / outputHeight;
  nextOutputBoundary = scaleY;
  rowSums = makeUniqueNoThrow<uint32_t[]>(outputWidth);
  if (!rowSums) {
    LOG_ERR("BMP", "OOM: scaling accumulator (%u bytes)", outputWidth * sizeof(uint32_t));
    return false;
  }
  if (!output.getScratchRow()) return false;
  return true;
}

bool GrayRowScaler::writeRow(const uint8_t* sourceRow, const int sourceY) {
  if (!output || !sourceRow) return false;
  if (!rowSums) return output->writeRow(sourceRow, sourceY);

  for (int outputX = 0; outputX < outputWidth; outputX++) {
    const int sourceStart = (static_cast<uint32_t>(outputX) * scaleX) >> 16;
    int sourceEnd = (static_cast<uint32_t>(outputX + 1) * scaleX) >> 16;
    if (sourceEnd > sourceWidth) sourceEnd = sourceWidth;
    if (sourceEnd <= sourceStart) sourceEnd = sourceStart + 1;
    for (int sourceX = sourceStart; sourceX < sourceEnd; sourceX++) rowSums[outputX] += sourceRow[sourceX];
  }
  accumulatedSourceRows++;

  const uint32_t sourceBoundary = static_cast<uint32_t>(sourceY + 1) << 16;
  while (sourceBoundary >= nextOutputBoundary && currentOutputY < outputHeight) {
    uint8_t* outputRow = output->getScratchRow();
    for (int outputX = 0; outputX < outputWidth; outputX++) {
      const int sourceStart = (static_cast<uint32_t>(outputX) * scaleX) >> 16;
      int sourceEnd = (static_cast<uint32_t>(outputX + 1) * scaleX) >> 16;
      if (sourceEnd > sourceWidth) sourceEnd = sourceWidth;
      if (sourceEnd <= sourceStart) sourceEnd = sourceStart + 1;
      outputRow[outputX] = rowSums[outputX] / ((sourceEnd - sourceStart) * accumulatedSourceRows);
    }
    if (!output->writeRow(outputRow, currentOutputY++)) return false;

    nextOutputBoundary = static_cast<uint32_t>(currentOutputY + 1) * scaleY;
    if (sourceBoundary >= nextOutputBoundary) continue;
    memset(rowSums.get(), 0, outputWidth * sizeof(uint32_t));
    accumulatedSourceRows = 0;
  }
  return true;
}
