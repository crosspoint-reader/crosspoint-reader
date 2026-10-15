#pragma once

#include <cstdint>
#include <memory>

#include "EpdFontData.h"
#include "FontBytes.h"
#include "TtfFace.h"

namespace ttf {

/// What became of a glyph (spec 6.1).
enum class Status : uint8_t {
  Ok,           ///< loaded or drawn; a glyph with no outline (a space) loads with width and height 0
  Unsupported,  ///< permanent: a point-matched or scaled-offset component, more points or contours than the
                ///< scratch block holds, too deep or too many components, a bitmap over 255 px a side or
                ///< over the output region, or coordinates out of range
  Malformed,    ///< permanent: broken glyph data (a read past the glyph or the file, contour ends out of
                ///< order, a flag run past the last point, a glyph index past numGlyphs)
  Transient,    ///< nothing is known to be wrong with the glyph (an SD read failed, Stage 2c; no scratch block;
                ///< or a size larger than the block was planned for): not marked in the store, tried again
};

constexpr bool isPermanent(const Status status) { return status == Status::Unsupported || status == Status::Malformed; }
const char* statusName(Status status);

constexpr uint16_t kMaxScratchPoints = 512;    ///< about 4.6 KB on the C3 (9 B a point)
constexpr uint16_t kMaxScratchContours = 256;  ///< 2 B a contour
constexpr uint32_t kMaxOutputBytes = 4096;     ///< the 2-bit output region
constexpr uint32_t kPoolBytesFlash = 4096;     ///< ftgrays' cell pool for a family in flash mode
constexpr uint32_t kPoolBytesSd = 8192;        ///< ... and in SD mode (CJK: about 37% faster than 4 KB)
constexpr uint32_t kMaxPpem26_6 = 255 << 6;    ///< larger sizes cannot fit EpdGlyph's 8-bit width and height

/// The parts of the scratch block (spec 5.3).
struct ScratchPlan {
  uint16_t points = 0;
  uint16_t contours = 0;
  uint32_t outputBytes = 0;
  uint32_t poolBytes = 0;
  /// The largest size the output region was sized for. A glyph too big for the region at a larger size is
  /// the plan's fault, not the glyph's, so it reports Transient rather than Unsupported.
  uint32_t maxPpem26_6 = kMaxPpem26_6;
};

/// The plan for one face: points and contours from maxp; the output from head's box at `maxPpem26_6` (the
/// largest size the family renders), but never less than the em square, since head's box can lie; each
/// capped. A glyph past a cap at a planned size reports Unsupported.
ScratchPlan planScratch(const Face& face, uint32_t maxPpem26_6, uint32_t poolBytes);
/// The larger of each part, so one block serves every style of a family.
ScratchPlan mergePlans(const ScratchPlan& a, const ScratchPlan& b);

/// Glyph outlines to 2-bit bitmaps: FreeType's TrueType loader rebuilt in integers (simple glyphs, compound
/// glyphs with offsets and 2.14 transforms, unhinted, exactly as FreeType scales them) feeding FreeType's own
/// anti-aliasing rasterizer (ftgrays), whose coverage spans are packed straight into the 2-bit stream with the
/// converter's thresholds. One per loaded family; it owns the family's scratch block.
///
/// Two steps, so a caller can learn a glyph's size before choosing where it goes: load() reads and scales
/// the outline into the scratch block and reports the metrics; rasterize() draws it anywhere.
///
/// Not reentrant across instances: ftgrays' standalone raster object is a static, so renders must not overlap.
/// The engine renders only under RenderLock.
class Rasterizer {
 public:
  Rasterizer() = default;
  Rasterizer(const Rasterizer&) = delete;
  Rasterizer& operator=(const Rasterizer&) = delete;

  /// Allocates the scratch block (one heap block, held until release()). False on OOM, logged.
  bool allocate(const ScratchPlan& plan);
  void release();
  bool allocated() const { return block_ != nullptr; }
  uint32_t scratchBytes() const { return blockBytes_; }
  const ScratchPlan& plan() const { return plan_; }
  /// The output region, plan().outputBytes long: render() leaves its bitmap here.
  uint8_t* output() { return block_ ? block_.get() + outputOffset_ : nullptr; }

  /// Loads `glyph` of `face` at `ppem26_6` into the scratch block and fills `metrics`: width, height, left
  /// and top of FreeType's unhinted bitmap, advanceX (12.4) as the .cpfont converter derives it, and
  /// dataLength; dataOffset is 0. The converter auto-hints its bitmaps, so only advances and line metrics
  /// match a .cpfont of the same font; the boxes and pixels match FreeType unhinted.
  Status load(FontBytes& bytes, const Face& face, uint16_t glyph, uint32_t ppem26_6, EpdGlyph* metrics);
  /// Draws the glyph of the last load() into `dst`, writing all of its dataLength bytes (MSB-first, rows
  /// continuous, as .cpfont bitmaps). After a failed load() it writes nothing and returns that status.
  Status rasterize(uint8_t* dst);
  /// load(), then rasterize() into output().
  Status render(FontBytes& bytes, const Face& face, uint16_t glyph, uint32_t ppem26_6, EpdGlyph* metrics);

 private:
  struct Loader;

  std::unique_ptr<uint8_t[]> block_;
  uint32_t blockBytes_ = 0;
  ScratchPlan plan_;
  uint32_t pointsOffset_ = 0;
  uint32_t contoursOffset_ = 0;
  uint32_t tagsOffset_ = 0;
  uint32_t outputOffset_ = 0;
  // The last load(): its status, outline size and bitmap box.
  Status loaded_ = Status::Malformed;
  uint16_t numPoints_ = 0;
  uint16_t numContours_ = 0;
  uint16_t width_ = 0;
  uint16_t height_ = 0;
};

}  // namespace ttf
