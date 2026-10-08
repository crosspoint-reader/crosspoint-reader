#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

#include "Block.h"
#include "Epub/converters/DecodeTarget.h"

class ImageBlock final : public Block {
 public:
  ImageBlock(const std::string& imagePath, const std::string& srcPath, int16_t width, int16_t height);
  ~ImageBlock() override = default;

  const std::string& getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

  bool imageExists() const;
  bool hasValidCache() const;
  bool needsDecode() const;

  // Cache-only decode of this image, run by ImageCacheService on its worker
  // task. The target carries the renderer geometry (writeFramebuffer=false) so
  // the worker never touches the live renderer. True when the .pxc is valid
  // afterwards.
  bool prefetch(const DecodeTarget& target, int x, int y) const;
  void renderPlaceholder(GfxRenderer& renderer, int x, int y) const;
  static void clearRenderFailures();

  // A page render draws its image up to ~13 times (BW double-refresh plus every
  // grayscale band pass), and each draw streams the whole .pxc off SD. The
  // first draw caches the pixel payload in RAM (chunked, heap-gated, falls back
  // to streaming when it doesn't fit); the reader calls this when the page
  // render completes so nothing stays resident between pages.
  static void releaseRenderCache();

  // Lazy extraction hook: the section build only header-probes images for their
  // dimensions; the file at imagePath is extracted out of the book on first
  // render, via this callback (function pointer + context, not std::function —
  // this is render-loop code). Registered by the reader activity that owns the
  // Epub, cleared on its exit.
  using ExtractFn = bool (*)(void* ctx, const char* srcPath, const char* destPath);
  static void setExtractor(void* ctx, ExtractFn fn);

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  void render(GfxRenderer& renderer, const int x, const int y);
  bool serialize(HalFile& file);
  static std::unique_ptr<ImageBlock> deserialize(HalFile& file);

 private:
  std::string imagePath;
  std::string srcPath;  // book-internal source href; empty once known-extracted
  int16_t width;
  int16_t height;

  bool positionOnScreen(int screenWidth, int screenHeight, int x, int y) const;
  // Lazy-extract from the book, validate, and decode straight to the .pxc (the
  // target disables framebuffer output); owned by ImageCacheService.
  bool decodeImage(const DecodeTarget& target, int x, int y, const std::string& cachePath) const;

  static void* extractCtx;
  static ExtractFn extractFn;
};
