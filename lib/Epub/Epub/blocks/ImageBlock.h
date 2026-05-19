#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

#include "Block.h"

// Source pixel area above which an image is considered "large" and rendered
// as a placeholder until the user explicitly requests it.
// 800x600 covers most full-page illustrations that take >1s to dither on ESP32.
static constexpr int32_t LARGE_IMAGE_PIXEL_THRESHOLD = 800 * 600;

class ImageBlock final : public Block {
 public:
  ImageBlock(const std::string& imagePath, int16_t width, int16_t height);
  ~ImageBlock() override = default;

  const std::string& getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

  bool imageExists() const;

  // Returns true if the source image dimensions exceed LARGE_IMAGE_PIXEL_THRESHOLD.
  // Result is cached after the first call to avoid repeated header reads.
  bool isLargeImage() const;

  // Returns true if this image would be shown as a placeholder given forceLoad.
  // False when: forceLoad is true, image is not large, or pixel cache already exists.
  bool wouldShowPlaceholder(bool forceLoad) const;

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  void render(GfxRenderer& renderer, int x, int y, bool forceLoad = true);
  bool serialize(FsFile& file);
  static std::unique_ptr<ImageBlock> deserialize(FsFile& file);

 private:
  std::string imagePath;
  int16_t width;
  int16_t height;

  mutable int8_t largeImageCached = 0;  // 0=unchecked, 1=large, -1=not large

  void renderPlaceholder(GfxRenderer& renderer, int x, int y) const;
};
