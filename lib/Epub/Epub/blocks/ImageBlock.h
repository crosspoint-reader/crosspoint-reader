#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

#include "Block.h"

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
  void renderPlaceholder(GfxRenderer& renderer, int x, int y) const;
  static void clearSessionRenderFailures();

  // A page render draws its image up to ~13 times (BW double-refresh plus every
  // grayscale band pass), and each draw streams the whole .pxc off SD. The
  // first draw caches the pixel payload in RAM (chunked, heap-gated, falls back
  // to streaming when it doesn't fit); the reader calls this when the page
  // render completes so nothing stays resident between pages.
  // With CROSSPOINT_PSRAM_IMAGE_CACHE the payload lives in a PSRAM LRU that
  // deliberately outlives the page render, and this becomes a no-op.
  static void releaseRenderCache();

#ifdef CROSSPOINT_PSRAM_IMAGE_CACHE
  // --- Whole-image PSRAM render cache -----------------------------------------
  // The format itself, and the decode-side donation entry point, live in
  // converters/PxcFormat.h. What belongs to the block layer is the slot set:
  //
  // Free every slot. The cache is bounded (8 x up to ~96 KB = ~750 KB of PSRAM)
  // and never evicts on its own beyond LRU, because its whole point is
  // surviving page turns. Leaving the reader ends that: the reader calls this
  // from onExit to give the PSRAM back, and the next book starts cold rather
  // than displacing slot by slot. Caller must hold the RenderLock (the cache
  // has no lock of its own).
  static void clearPxcLru();
#endif

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

  static void* extractCtx;
  static ExtractFn extractFn;
};
