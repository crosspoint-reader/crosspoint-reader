#pragma once
#include <HalStorage.h>
#include <stdint.h>

#include <memory>
#include <string>

#include "Block.h"

class BuildArena;  // lib/Memory — optional decode scratch (see image_scratch below)

// Source file size above which an image is considered "large" and rendered as a placeholder
// until the user explicitly requests it.
//
// This used to be a PIXEL area (800*600), compared against ImageBlock's width/height — which are
// the DISPLAY dimensions, not the source's. On a 480x800 panel the largest image that can be
// drawn covers 384000 pixels, so the test could never be true and the placeholder never appeared,
// whatever the setting said. Bytes also happen to be the better predictor: what costs seconds is
// pulling the entry out of the ZIP and inflating it, and that scales with the file, not with the
// area it ends up occupying on screen.
//
// 256 KB, from device measurements on X4: Men at Arms' 857182-byte cover PNG cost ~17 s to
// extract plus ~5.8 s per decode pass, while the same book's 113588-byte and 155096-byte JPEGs
// are unremarkable. The line wants to sit above the second group and well below the first.
//
// Note what this does NOT cover: a small file that inflates to an enormous bitmap (a flat-colour
// 5000x5000 PNG) is cheap to read and slow to decode, and nothing here sees that. Source pixel
// dimensions are known at parse time but are not carried on the block; if such a book turns up,
// that is the axis to add rather than moving this number.
static constexpr uint32_t LARGE_IMAGE_SOURCE_BYTES = 256 * 1024;

// Process-wide scratch arena for image decoding, installed for the duration of a multi-image
// pass (see Section::warmAllImageCaches).
//
// Each decode otherwise takes and returns its own large blocks — a 32 KB inflate ring (PNG) or
// a 12 KB work pool (JPEG), plus scanline buffers — and nothing is reused between images. A
// warm pass decoding N images therefore performs N rounds of that churn, which on a
// no-compaction heap breaks up the contiguous region a later framebuffer realloc needs. See
// docs/memory-allocation-strategy.md (rule 4) and the crash it explains.
//
// A global rather than a parameter because the arena has to reach decoders several layers down
// (Section -> Page -> ImageBlock -> converter -> PngStreamDecoder) through interfaces shared
// with callers that have no arena. Decoders fall back to the heap when none is installed, so
// this is an optimisation, never a requirement.
namespace image_scratch {
// The installed arena, or nullptr. Not owned.
BuildArena* get();
// Install/clear. The arena must outlive the scope; use ScopedArena rather than calling these.
void set(BuildArena* arena);

// True when the installed arena has room to serve `bytes`, i.e. the heap will NOT be asked for
// them. Free-heap floors that exist to cover an arena-capable block MUST discount it through
// this, or they double-count: MIN_FREE_HEAP_FOR_JPEG is literally the work-pool size plus
// headroom, so with an arena installed it demands 12 KB of heap for a block the heap never
// sees, and refuses decodes the heap could easily serve.
//
// Mirrors what the allocators actually do (they bump-allocate and fall back to the heap on
// refusal), including worst-case alignment padding. An over-optimistic answer degrades to a
// heap fallback inside the decoder, which is already handled — it cannot crash.
bool canServe(size_t bytes);

// A decode refused for the heap, or cut short by an allocation that failed (its pixel cache included), or one that
// published a coarse picture for lack of it (noteCoarse), says so here: the reader's memory ladder (ImageMemoryLadder)
// retries such a decode with more memory, and leaves one that failed for any other reason alone. A count, so a caller
// compares it before and after its decodes.
void noteHeapShort();
uint32_t heapShortCount();

// What the next finer scale of a decode that published a coarse picture for lack of memory would take: a workspace of
// `bytes`, from the arena, or from the heap with `heapFloor` more left free (PixelCache::PXC_MAGIC_COARSE). 0: none.
struct CoarseNeed {
  uint32_t bytes = 0;
  uint32_t heapFloor = 0;
};
// A decode published a coarse picture: short of memory (noteHeapShort), and this much from a finer one. The largest
// need since the last takeCoarseNeed() is kept, so the ladder never decodes again for a picture that cannot improve.
void noteCoarse(const CoarseNeed& need);
CoarseNeed takeCoarseNeed();
// Whether `need` can be had now, the decoder's way: the arena has room for it, or the heap a block of it and its floor
// beside; with `freedFirst` more bytes counted as free in both (what a relief step would free).
bool coarseNeedFits(const CoarseNeed& need, uint32_t freedFirst = 0);

// RAII installer — restores the previous value, so nesting is safe.
class ScopedArena {
 public:
  explicit ScopedArena(BuildArena* arena) : previous_(get()) { set(arena); }
  ~ScopedArena() { set(previous_); }
  ScopedArena(const ScopedArena&) = delete;
  ScopedArena& operator=(const ScopedArena&) = delete;

 private:
  BuildArena* previous_;
};
}  // namespace image_scratch

class ImageBlock final : public Block {
 public:
  // The alt text is kept to MAX_ALT_TEXT_BYTES (see there).
  ImageBlock(std::string imagePath, int16_t width, int16_t height, std::string altText = "");
  // Extended constructor used when lazy extraction is desired:
  // imagePath is the SD cache destination; epubFilePath + epubEntryPath are the source.
  ImageBlock(std::string imagePath, int16_t width, int16_t height, std::string altText, std::string epubFilePath,
             std::string epubEntryPath);

  // The alt text is drawn on one line of the placeholder and nowhere else, so this is all of it
  // that can ever show. An accessibility description can run to KBs, and an image block holds
  // its alt for as long as its page is loaded -- every draw, pre-render and image-lane visit.
  static constexpr size_t MAX_ALT_TEXT_BYTES = 128;
  ~ImageBlock() override = default;

  // Return a new ImageBlock that renders a vertical crop of this image.
  // srcYOffset: first source row to render; srcHeight: number of rows (must be > 0).
  // The new block shares the same imagePath (and thus the same decoded cache file);
  // only the rendering window differs.
  std::unique_ptr<ImageBlock> makeCrop(int16_t srcYOffset, int16_t srcHeight) const;

  const std::string& getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }
  // Rendered height: equals srcHeight when a crop is active, otherwise height.
  int16_t getRenderedHeight() const { return srcHeight_ > 0 ? srcHeight_ : height; }
  const std::string& getAltText() const { return altText; }

  bool imageExists() const;

  // Returns true if the source image's file size exceeds LARGE_IMAGE_SOURCE_BYTES. Answered from
  // the extracted file when there is one, otherwise from the ZIP entry's uncompressed size (one
  // central-directory scan). Result is cached after the first call: the lookup is only ever
  // reached on a pixel-cache miss, i.e. immediately before a decode that costs orders of
  // magnitude more, but a page can render several times.
  bool isLargeImage() const;

  // Returns true if this image would be shown as a placeholder given forceLoad.
  // False when: forceLoad is true, image is not large, or the mode-specific cache exists.
  // monochromeOutput selects which cache to check (BW or grayscale), matching render().
  bool wouldShowPlaceholder(bool forceLoad, bool monochromeOutput) const;

  // True when the 1-bit .pxc pixel cache exists (BW plane rendering).
  // Used by warm-cache paths to skip already-cached images.
  bool hasPixelCache() const;

  // True when the 4-level .pxc pixel cache exists (grayscale AA rendering).
  bool hasGrayscaleCache() const;

  // Render the 4-level cache into the framebuffer using the renderer's current
  // mode (GRAYSCALE_LSB or GRAYSCALE_MSB). No-op if no grayscale cache exists.
  // Called by the AA grayscale passes to give images proper gray tones.
  void renderGrayscaleFromCache(GfxRenderer& renderer, int x, int y) const;

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  // monochromeOutput=true: 1-bit Atkinson dither → BW-plane rendering (AA off)
  // monochromeOutput=false: 4-level Bayer dither → also replayed in grayscale passes (AA on)
  // alsoCacheOtherVariant: on a decode (cache miss), write the OTHER dither variant's .pxc in
  // the same pass as well. Saves the second full inflate when the caller knows it needs both --
  // see Page::warmImageCaches. Ignored when the render is served from cache or a placeholder,
  // and best-effort: only the PNG decoder honours it, so callers must re-check.
  void render(GfxRenderer& renderer, int x, int y, bool forceLoad = true, bool monochromeOutput = true,
              bool alsoCacheOtherVariant = false);
  bool serialize(FsFile& file);
  static std::unique_ptr<ImageBlock> deserialize(FsFile& file);

 private:
  std::string imagePath;  // SD card destination path (may not exist until first render)
  std::string altText;
  int16_t width;
  int16_t height;
  // Vertical crop window into the decoded image. srcYOffset_==0 && srcHeight_==0 means full image.
  int16_t srcYOffset_ = 0;
  int16_t srcHeight_ = 0;
  // Cached isLargeImage() answer: -1 not yet resolved, 0 no, 1 yes. Mutable because the query is
  // const and the answer cannot change for the life of the block.
  mutable int8_t largeImageCached_ = -1;
  std::string epubFilePath_;   // source EPUB on SD (empty if already extracted)
  std::string epubEntryPath_;  // internal EPUB entry path (e.g. "OEBPS/images/foo.jpg")

  // Ensure the SD cache file exists, extracting from the EPUB if necessary.
  // Returns true if the file is ready for decoding.
  bool ensureExtracted() const;

  // CoarseRedecodeScope's halves, for the variant `monochromeOutput` selects: the cache moved aside when stamped
  // coarse (true when it was), and moved back unless a cache has taken its place.
  bool setCoarseCacheAside(bool monochromeOutput) const;
  void restoreCoarseCache(bool monochromeOutput) const;

 public:
  // What a placeholder box says beside the alt text: "large image, press to load", the image is still being
  // prepared (a mid-build draw), or there was not memory enough to decode it.
  enum class Placeholder : uint8_t { Large, Loading, LowMemory };

  // The LowMemory placeholders drawn since the last call: the reader shows its low-memory indicator while the page
  // on screen has one.
  static uint16_t takeLowMemoryPlaceholders() {
    const uint16_t drawn = lowMemoryDrawn_;
    lowMemoryDrawn_ = 0;
    return drawn;
  }

 private:
  void renderPlaceholder(GfxRenderer& renderer, int x, int y, Placeholder kind) const;
  static bool placeholderOnly_;
  static Placeholder placeholderOnlyKind_;
  static uint16_t placeholdersDrawn_;
  static uint16_t lowMemoryDrawn_;

 public:
  // While one of these is live, render() draws every image that is not already in its pixel
  // cache as a "loading" placeholder instead of decoding it. The mid-build page draw uses it:
  // a page the reader is waiting on can be shown the moment its text exists, with the
  // decode left to the normal render once the build completes. Decoding there would run on
  // the build's starved heap while the secondary buffer is lent to the build.
  //
  // placeholdersDrawn() is for a caller that must not show a page with a placeholder on it at
  // all (the next-page pre-render): a cache that exists can still fail to replay -- a stale
  // version is deleted on read, a width mismatch is refused -- and only the draw finds out.
  //
  // `kind` LowMemory: the reader's memory ladder found that no decode of these images fits (see
  // ImageMemoryLadder), so the page is drawn with them as "not enough memory" boxes instead.
  struct PlaceholderOnlyScope {
    explicit PlaceholderOnlyScope(const Placeholder kind = Placeholder::Loading) {
      placeholderOnly_ = true;
      placeholderOnlyKind_ = kind;
      placeholdersDrawn_ = 0;
    }
    ~PlaceholderOnlyScope() { placeholderOnly_ = false; }
    PlaceholderOnlyScope(const PlaceholderOnlyScope&) = delete;
    PlaceholderOnlyScope& operator=(const PlaceholderOnlyScope&) = delete;
    [[nodiscard]] uint16_t placeholdersDrawn() const { return placeholdersDrawn_; }
  };

  // While one of these is live, `image`'s pixel cache is out of the way when it was stamped coarse
  // (PixelCache::PXC_MAGIC_COARSE: a decode that settled for a lower scale or the DC preview because the heap was
  // short), so a render decodes the image again; at its end the coarse cache comes back unless that decode wrote one
  // in its place. A decode that fails, or stops for input, leaves the coarse picture, never none, and nothing it
  // parked (no pass would resume it). For the variant `monochromeOutput` selects, and with `alsoGrayscale` the
  // 4-level one beside a 1-bit too. Only for a pass with memory to spare (Page::warmImageCaches' redecodeCoarse: the
  // pass before a heap restart, the memory ladder's light step where the finer workspace fits).
  class CoarseRedecodeScope {
   public:
    CoarseRedecodeScope(const ImageBlock& image, const bool monochromeOutput, const bool alsoGrayscale)
        : image_(image),
          monochromeOutput_(monochromeOutput),
          primaryAside_(image.setCoarseCacheAside(monochromeOutput)),
          grayscaleAside_(alsoGrayscale && image.setCoarseCacheAside(false)) {}
    ~CoarseRedecodeScope() {
      if (primaryAside_) image_.restoreCoarseCache(monochromeOutput_);
      if (grayscaleAside_) image_.restoreCoarseCache(false);
    }
    CoarseRedecodeScope(const CoarseRedecodeScope&) = delete;
    CoarseRedecodeScope& operator=(const CoarseRedecodeScope&) = delete;

   private:
    const ImageBlock& image_;
    bool monochromeOutput_ = true;
    bool primaryAside_ = false;
    bool grayscaleAside_ = false;
  };
};
