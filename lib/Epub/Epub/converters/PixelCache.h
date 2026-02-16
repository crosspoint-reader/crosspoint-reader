#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <stdint.h>

#include <cstring>
#include <string>

// Cache buffer for storing 2-bit pixels (4 levels) during decode.
// Packs 4 pixels per byte, MSB first.
struct PixelCache {
  uint8_t* buffer;
  int width;
  int height;
  int bytesPerRow;
  int originX;  // config.x - to convert screen coords to cache coords
  int originY;  // config.y

  PixelCache() : buffer(nullptr), width(0), height(0), bytesPerRow(0), originX(0), originY(0) {}
  PixelCache(const PixelCache&) = delete;
  PixelCache& operator=(const PixelCache&) = delete;

  static constexpr size_t MAX_CACHE_BYTES = 256 * 1024;  // 256KB limit for embedded targets

  bool allocate(int w, int h, int ox, int oy) {
    width = w;
    height = h;
    originX = ox;
    originY = oy;
    bytesPerRow = (w + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
    size_t bufferSize = (size_t)bytesPerRow * h;
    if (bufferSize > MAX_CACHE_BYTES) {
      LOG_ERR("IMG", "Cache buffer too large: %d bytes for %dx%d (limit %d)", bufferSize, w, h, MAX_CACHE_BYTES);
      return false;
    }
    buffer = (uint8_t*)malloc(bufferSize);
    if (buffer) {
      memset(buffer, 0, bufferSize);
      LOG_DBG("IMG", "Allocated cache buffer: %d bytes for %dx%d", bufferSize, w, h);
    }
    return buffer != nullptr;
  }

  void setPixel(int screenX, int screenY, uint8_t value) {
    if (!buffer) return;
    int localX = screenX - originX;
    int localY = screenY - originY;
    if (localX < 0 || localX >= width || localY < 0 || localY >= height) return;

    int byteIdx = localY * bytesPerRow + localX / 4;
    int bitShift = 6 - (localX % 4) * 2;  // MSB first: pixel 0 at bits 6-7
    buffer[byteIdx] = (buffer[byteIdx] & ~(0x03 << bitShift)) | ((value & 0x03) << bitShift);
  }

  bool writeToFile(const std::string& cachePath) {
    if (!buffer) return false;

    FsFile cacheFile;
    if (!Storage.openFileForWrite("IMG", cachePath, cacheFile)) {
      LOG_ERR("IMG", "Failed to open cache file for writing: %s", cachePath.c_str());
      return false;
    }

    uint16_t w = width;
    uint16_t h = height;
    cacheFile.write(&w, 2);
    cacheFile.write(&h, 2);
    cacheFile.write(buffer, bytesPerRow * height);
    cacheFile.close();

    LOG_DBG("IMG", "Cache written: %s (%dx%d, %d bytes)", cachePath.c_str(), width, height, 4 + bytesPerRow * height);
    return true;
  }

  ~PixelCache() {
    if (buffer) {
      free(buffer);
      buffer = nullptr;
    }
  }
};
