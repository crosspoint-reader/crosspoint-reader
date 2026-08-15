#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

#include "Block.h"

#ifdef CROSSPOINT_BG_IMAGE_DECODE
// Only ever a pointer here: the background pre-decode resolves its decoder
// under the RenderLock and hands it back for the unlocked decode.
class ImageToFramebufferDecoder;
#endif

class ImageBlock final : public Block {
 public:
  ImageBlock(const std::string& imagePath, const std::string& srcPath, int16_t width, int16_t height);
  ~ImageBlock() override = default;

  const std::string& getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }
#ifdef CROSSPOINT_BG_IMAGE_DECODE
  // Book-internal href, for a caller that extracts the image itself (see
  // decodeToCacheOnly). Empty once the file is known to be extracted.
  const std::string& getSourcePath() const { return srcPath; }
#endif

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
  static void releaseRenderCache();

#ifdef CROSSPOINT_BG_IMAGE_DECODE
  // --- Background pre-decode ---------------------------------------------------
  // The reader pre-decodes images on upcoming pages from its background task so
  // the first view of an image page finds a ready .pxc instead of paying a
  // multi-second decode on the page-turn critical path. That decode runs with
  // NO RenderLock held; these statics are what make it safe. The full argument
  // lives next to their definitions in ImageBlock.cpp.

  // Under the RenderLock: the decoder for this format, or null when there is
  // none. Resolving it here is not just a capability test -- it is also how
  // ImageDecoderFactory's lazy singleton creation stays serialized by the lock
  // (that lazy init is not thread-safe). The caller carries the returned
  // pointer into its unlocked decode (decodeToCacheOnly) instead of asking the
  // factory again from there; the singletons are never destroyed, so the
  // pointer stays valid.
  static ImageToFramebufferDecoder* backgroundDecoderFor(const std::string& imagePath);

  // Under the RenderLock, immediately before releasing it: publish that a
  // background decode of `imagePath` is starting. Cleared by the background
  // task when the decode ends, wherever it ends.
  static void beginBackgroundDecode(const std::string& imagePath);
  static void endBackgroundDecode();

  // True when a background decode is in flight AND it is producing exactly this
  // .pxc. Lets a render take the image over before it opens that file for read,
  // without paying a cancel-and-wait for images the background task is not
  // touching. Read from the render task (RenderLock held).
  static bool backgroundDecodeTargets(const std::string& cachePath);

  // Stop any in-flight background decode and wait (bounded) for it to
  // acknowledge. Used by render() before it decodes, and by the reader before
  // it joins the background task.
  enum class BgDecodeCancel {
    None,      // nothing was running
    Stopped,   // it stopped (or had just finished) -- its .pxc may now exist
    TimedOut,  // it did not stop; the caller must NOT start a decode of its own
  };
  static BgDecodeCancel cancelBackgroundDecode();

  // With NO RenderLock held: decode straight into this image's .pxc, touching
  // neither the renderer nor the framebuffer. `renderer` is forwarded to the
  // decoder interface and never dereferenced. `decoder` is the pointer the
  // caller resolved under the lock (see backgroundDecoderFor). The geometry
  // must be the one a render would use -- both the screen clip and the dither
  // phase depend on absolute position -- so the file this writes is the file a
  // render wants.
  static bool decodeToCacheOnly(GfxRenderer& renderer, ImageToFramebufferDecoder* decoder, const std::string& imagePath,
                                int x, int y, int width, int height, int screenWidth, int screenHeight);
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
