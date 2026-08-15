#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

class GfxRenderer;

struct ImageDimensions {
  int16_t width;
  int16_t height;
};

struct RenderConfig {
  int x, y;
  int maxWidth, maxHeight;
  bool useGrayscale = true;
  bool useDithering = true;
  bool performanceMode = false;
  bool useExactDimensions = false;  // If true, use maxWidth/maxHeight as exact output size (no recalculation)
  float sourceCropX = 0.0f;         // Fraction cropped equally from the left and right edges
  float sourceCropY = 0.0f;         // Fraction cropped equally from the top and bottom edges
  bool preserveAlpha = false;       // Skip transparent pixels instead of compositing them against white
  std::string cachePath;            // If non-empty, decoder will write pixel cache to this path
#ifdef CROSSPOINT_BG_IMAGE_DECODE
  // Produce ONLY the pixel cache: no framebuffer writes, and no interaction
  // with the renderer whatsoever. The background pre-decode runs on the
  // reader's build task with no RenderLock held, so it may not read renderer
  // state either -- which is why the screen bounds the decode clips against are
  // passed here, captured by the caller while it still held the lock. Requires
  // a cachePath (a cacheOnly decode with nowhere to write produces nothing).
  bool cacheOnly = false;
  int screenWidth = 0;
  int screenHeight = 0;
#endif
};

class ImageToFramebufferDecoder {
 public:
  virtual ~ImageToFramebufferDecoder() = default;

  virtual bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) = 0;

  virtual bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const = 0;

  virtual const char* getFormatName() const = 0;

#ifdef CROSSPOINT_BG_IMAGE_DECODE
  // Cooperative abort, checked in the converters' per-MCU / per-scanline draw
  // callbacks and in the pixel cache's write path, so a decode in flight gives
  // up within one callback instead of running to completion. The decode then
  // reports failure and drops its partial .pxc.
  //
  // Deliberately one global flag rather than per-decode state: both users --
  // the reader's exit join, and the render task taking an image over from the
  // background task -- raise it while holding the RenderLock and lower it
  // before releasing, and no render-task decode can be in flight at either
  // moment, so the two can never overlap.
  static void requestAbort(bool abort);
  static bool abortRequested();
#endif

 protected:
  // Size validation helpers
  static constexpr int MAX_SOURCE_PIXELS = 3145728;  // 2048 * 1536

  bool validateImageDimensions(int width, int height, const std::string& format);
  void warnUnsupportedFeature(const std::string& feature, const std::string& imagePath);
};
