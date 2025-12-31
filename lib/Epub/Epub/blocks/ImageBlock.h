#pragma once

#include <string>

#include "Block.h"

class GfxRenderer;

// Represents an image block in the HTML document
class ImageBlock final : public Block {
 public:
  std::string cachePath;  // Path to cached BMP file
  int width;
  int height;
  bool isCached;

  ImageBlock() : width(0), height(0), isCached(false) {}
  ImageBlock(std::string path, int w, int h)
      : cachePath(std::move(path)), width(w), height(h), isCached(true) {}

  void layout(GfxRenderer& renderer) override;
  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return cachePath.empty() || !isCached; }
  void finish() override {}

  // Get scaled dimensions that fit within viewport
  void getScaledDimensions(int viewportWidth, int viewportHeight, int* outWidth, int* outHeight) const;
};
