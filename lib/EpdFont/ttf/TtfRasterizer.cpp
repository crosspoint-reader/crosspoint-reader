#include "TtfRasterizer.h"

#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "TtfFixed.h"
#include "TtfGrays.h"

namespace ttf {

namespace {

// Simple-glyph flags.
constexpr uint8_t kOnCurve = 0x01;
constexpr uint8_t kXShort = 0x02;
constexpr uint8_t kYShort = 0x04;
constexpr uint8_t kRepeat = 0x08;
constexpr uint8_t kXSameOrPositive = 0x10;
constexpr uint8_t kYSameOrPositive = 0x20;

// Compound-glyph component flags.
constexpr uint16_t kArgsAreWords = 0x0001;
constexpr uint16_t kArgsAreXy = 0x0002;
constexpr uint16_t kHaveScale = 0x0008;
constexpr uint16_t kMoreComponents = 0x0020;
constexpr uint16_t kHaveXyScale = 0x0040;
constexpr uint16_t kHaveTwoByTwo = 0x0080;
constexpr uint16_t kUseMyMetrics = 0x0200;
constexpr uint16_t kScaledOffset = 0x0800;

constexpr int kMaxDepth = 8;                  // nesting below the top glyph; fonts use 1 to 3
constexpr uint16_t kMaxComponentLoads = 256;  // per glyph: a compound of compounds cannot run away
constexpr int64_t kMaxCoord = 1 << 24;        // 26.6, far past any glyph that fits; keeps every product in range

uint32_t alignUp(const uint32_t value, const uint32_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

// A glyph record: every read stays inside it, and the first that would not marks it overrun.
struct Cursor {
  FontBytes& bytes;
  uint32_t pos;
  uint32_t end;
  bool overrun = false;

  bool take(const uint32_t n) {
    if (n > end - pos) {
      overrun = true;
      pos = end;
      return false;
    }
    pos += n;
    return true;
  }
  uint8_t u8() { return take(1) ? bytes.u8(pos - 1) : 0; }
  uint16_t u16() { return take(2) ? bytes.u16(pos - 2) : 0; }
  int16_t i16() { return static_cast<int16_t>(u16()); }
};

bool inRange(const int64_t value) { return value >= -kMaxCoord && value <= kMaxCoord; }

// Packs ftgrays' coverage spans into the 2-bit stream with the PC converter's thresholds (coverage nibble
// >= 12 -> 3, >= 8 -> 2, >= 4 -> 1). ftgrays counts rows from the bottom; the bitmap starts at the top.
struct SpanSink {
  uint8_t* out;
  int width;
  int height;
};

void packSpans(const int y, const int count, const FT_Span* spans, void* user) {
  const auto* sink = static_cast<const SpanSink*>(user);
  const int row = sink->height - 1 - y;
  if (row < 0 || row >= sink->height) return;
  for (int i = 0; i < count; ++i) {
    const int level = spans[i].coverage >> 4;
    const uint8_t value = level >= 12 ? 3 : level >= 8 ? 2 : level >= 4 ? 1 : 0;
    if (value == 0) continue;
    const int from = spans[i].x < 0 ? 0 : spans[i].x;
    const int to = spans[i].x + spans[i].len < sink->width ? spans[i].x + spans[i].len : sink->width;
    for (int x = from; x < to; ++x) {
      const uint32_t pixel = static_cast<uint32_t>(row * sink->width + x);
      sink->out[pixel >> 2] |= static_cast<uint8_t>(value << (6 - 2 * (pixel & 3)));
    }
  }
}

}  // namespace

// Reads a glyph into the scratch arrays the way FreeType's TrueType loader does when it does not hint: each
// simple glyph's points scaled with FT_MulFix as they are read, each component transformed and then offset
// in 26.6, and the whole outline moved so the left side bearing's phantom point is the origin.
struct Rasterizer::Loader {
  FontBytes& bytes;
  const Face& face;
  int32_t scale;
  FT_Vector* points;
  unsigned short* contours;
  unsigned char* tags;
  uint16_t pointCap;
  uint16_t contourCap;
  uint16_t numPoints = 0;
  uint16_t numContours = 0;
  uint16_t componentLoads = 0;

  // Appends `glyph` and reports its phantom point pp1.x: the shift FreeType applies when it is the top glyph,
  // or a component flagged USE_MY_METRICS.
  Status load(const uint16_t glyph, const int depth, int32_t* pp1x) {
    if (glyph >= face.numGlyphs) return Status::Malformed;
    uint32_t start = 0;
    uint32_t end = 0;
    const uint32_t entry = static_cast<uint32_t>(glyph) * (face.longLoca ? 4 : 2);
    if (entry + (face.longLoca ? 8 : 4) > face.locaLength) return Status::Malformed;
    if (face.longLoca) {
      start = bytes.u32(face.loca + entry);
      end = bytes.u32(face.loca + entry + 4);
    } else {
      start = 2u * bytes.u16(face.loca + entry);
      end = 2u * bytes.u16(face.loca + entry + 2);
    }
    if (start > end || end > face.glyfLength) return Status::Malformed;

    uint16_t advance = 0;
    int16_t lsb = 0;
    horizontalMetrics(bytes, face, glyph, &advance, &lsb);
    if (start == end) {  // no outline, as a space; FreeType takes its box as 0
      *pp1x = mulFix(-lsb, scale);
      return Status::Ok;
    }
    Cursor cursor{bytes, face.glyf + start, face.glyf + end};
    const int16_t contourCount = cursor.i16();
    const int16_t xMin = cursor.i16();
    cursor.take(6);  // yMin, xMax, yMax
    if (cursor.overrun) return Status::Malformed;
    *pp1x = mulFix(xMin - lsb, scale);
    if (contourCount >= 0) return loadSimple(cursor, contourCount);
    if (depth >= kMaxDepth) return Status::Unsupported;
    return loadCompound(cursor, depth, pp1x);
  }

  Status loadSimple(Cursor& cursor, const int16_t count) {
    if (count == 0) return Status::Ok;
    if (count > contourCap - numContours) return Status::Unsupported;
    const uint16_t base = numPoints;
    int32_t last = -1;
    for (int16_t i = 0; i < count; ++i) {
      const uint16_t endPoint = cursor.u16();
      if (cursor.overrun || endPoint <= last) return Status::Malformed;  // FreeType: ends must increase
      if (endPoint >= pointCap - base) return Status::Unsupported;
      contours[numContours + i] = static_cast<unsigned short>(base + endPoint);
      last = endPoint;
    }
    const uint32_t count32 = static_cast<uint32_t>(last) + 1;
    cursor.take(cursor.u16());  // the instructions
    if (cursor.overrun) return Status::Malformed;

    unsigned char* flags = tags + base;
    for (uint32_t i = 0; i < count32;) {
      const uint8_t flag = cursor.u8();
      flags[i++] = flag;
      if (flag & kRepeat) {
        const uint8_t repeat = cursor.u8();
        if (repeat > count32 - i) return Status::Malformed;  // FreeType: a run past the last point
        for (uint8_t k = 0; k < repeat; ++k) flags[i++] = flag;
      }
      if (cursor.overrun) return Status::Malformed;
    }
    FT_Vector* out = points + base;
    int32_t value = 0;
    for (uint32_t i = 0; i < count32; ++i) {
      if (flags[i] & kXShort) {
        const int32_t delta = cursor.u8();
        value += (flags[i] & kXSameOrPositive) ? delta : -delta;
      } else if (!(flags[i] & kXSameOrPositive)) {
        value += cursor.i16();
      }
      out[i].x = value;
    }
    value = 0;
    for (uint32_t i = 0; i < count32; ++i) {
      if (flags[i] & kYShort) {
        const int32_t delta = cursor.u8();
        value += (flags[i] & kYSameOrPositive) ? delta : -delta;
      } else if (!(flags[i] & kYSameOrPositive)) {
        value += cursor.i16();
      }
      out[i].y = value;
    }
    if (cursor.overrun) return Status::Malformed;
    for (uint32_t i = 0; i < count32; ++i) {
      const int64_t x = mulFixWide(static_cast<int32_t>(out[i].x), scale);
      const int64_t y = mulFixWide(static_cast<int32_t>(out[i].y), scale);
      if (!inRange(x) || !inRange(y)) return Status::Unsupported;
      out[i].x = static_cast<FT_Pos>(x);
      out[i].y = static_cast<FT_Pos>(y);
      flags[i] = (flags[i] & kOnCurve) ? FT_CURVE_TAG_ON : FT_CURVE_TAG_CONIC;
    }
    numContours = static_cast<uint16_t>(numContours + count);
    numPoints = static_cast<uint16_t>(numPoints + count32);
    return Status::Ok;
  }

  Status loadCompound(Cursor& cursor, const int depth, int32_t* pp1x) {
    uint16_t flags = 0;
    do {
      if (++componentLoads > kMaxComponentLoads) return Status::Unsupported;
      flags = cursor.u16();
      const uint16_t child = cursor.u16();
      int32_t arg1 = 0;
      int32_t arg2 = 0;
      if (flags & kArgsAreWords) {
        arg1 = cursor.i16();
        arg2 = cursor.i16();
      } else {
        arg1 = static_cast<int8_t>(cursor.u8());
        arg2 = static_cast<int8_t>(cursor.u8());
      }
      // The 2.14 transform as FreeType holds it, in 16.16: x' = x*xx + y*xy, y' = x*yx + y*yy.
      int32_t xx = 0x10000;
      int32_t xy = 0;
      int32_t yx = 0;
      int32_t yy = 0x10000;
      if (flags & kHaveScale) {
        xx = yy = cursor.i16() * 4;
      } else if (flags & kHaveXyScale) {
        xx = cursor.i16() * 4;
        yy = cursor.i16() * 4;
      } else if (flags & kHaveTwoByTwo) {
        xx = cursor.i16() * 4;
        yx = cursor.i16() * 4;
        xy = cursor.i16() * 4;
        yy = cursor.i16() * 4;
      }
      if (cursor.overrun) return Status::Malformed;
      const bool transformed = (flags & (kHaveScale | kHaveXyScale | kHaveTwoByTwo)) != 0;
      if (!(flags & kArgsAreXy)) return Status::Unsupported;                   // point matching (spec 5.6)
      if (transformed && (flags & kScaledOffset)) return Status::Unsupported;  // Apple's scaled offsets

      const uint16_t first = numPoints;
      int32_t childPp1x = 0;
      const Status status = load(child, depth + 1, &childPp1x);
      if (status != Status::Ok) return status;
      if (transformed) {
        for (uint16_t i = first; i < numPoints; ++i) {
          const int32_t x = static_cast<int32_t>(points[i].x);
          const int32_t y = static_cast<int32_t>(points[i].y);
          const int64_t tx = mulFixWide(x, xx) + mulFixWide(y, xy);
          const int64_t ty = mulFixWide(x, yx) + mulFixWide(y, yy);
          if (!inRange(tx) || !inRange(ty)) return Status::Unsupported;
          points[i].x = static_cast<FT_Pos>(tx);
          points[i].y = static_cast<FT_Pos>(ty);
        }
      }
      if (!translate(first, mulFix(arg1, scale), mulFix(arg2, scale))) return Status::Unsupported;
      if (flags & kUseMyMetrics) *pp1x = childPp1x;
    } while (flags & kMoreComponents);
    return Status::Ok;
  }

  // Moves the points from `first` on; false when one would leave the coordinate range.
  bool translate(const uint16_t first, const int32_t dx, const int32_t dy) {
    if (dx == 0 && dy == 0) return true;
    for (uint16_t i = first; i < numPoints; ++i) {
      const int64_t x = static_cast<int64_t>(points[i].x) + dx;
      const int64_t y = static_cast<int64_t>(points[i].y) + dy;
      if (!inRange(x) || !inRange(y)) return false;
      points[i].x = static_cast<FT_Pos>(x);
      points[i].y = static_cast<FT_Pos>(y);
    }
    return true;
  }
};

const char* statusName(const Status status) {
  switch (status) {
    case Status::Ok:
      return "ok";
    case Status::Unsupported:
      return "unsupported";
    case Status::Malformed:
      return "malformed";
    case Status::Transient:
      return "transient";
  }
  return "?";
}

ScratchPlan planScratch(const Face& face, const uint32_t maxPpem26_6, const uint32_t poolBytes) {
  ScratchPlan plan;
  plan.points = face.maxPoints < kMaxScratchPoints ? face.maxPoints : kMaxScratchPoints;
  plan.contours = face.maxContours < kMaxScratchContours ? face.maxContours : kMaxScratchContours;
  plan.maxPpem26_6 = maxPpem26_6 < kMaxPpem26_6 ? maxPpem26_6 : kMaxPpem26_6;
  const int32_t scale = unitsToPixels(face, plan.maxPpem26_6);
  // head's box at that size, one pixel wider each way for the rounding of compound transforms. FreeType does
  // not trust head's box, so neither side is ever smaller than the em square.
  const int32_t em = ((mulFix(face.unitsPerEm, scale) + 63) >> 6) + 2;
  int32_t width = ((mulFix(face.xMax, scale) + 63) >> 6) - (mulFix(face.xMin, scale) >> 6) + 2;
  int32_t height = ((mulFix(face.yMax, scale) + 63) >> 6) - (mulFix(face.yMin, scale) >> 6) + 2;
  width = width < em ? em : width > 255 ? 255 : width;
  height = height < em ? em : height > 255 ? 255 : height;
  const uint32_t bytes = glyphDataBytes(width, height, true);
  plan.outputBytes = bytes < kMaxOutputBytes ? bytes : kMaxOutputBytes;
  plan.poolBytes = poolBytes;
  return plan;
}

ScratchPlan mergePlans(const ScratchPlan& a, const ScratchPlan& b) {
  ScratchPlan plan;
  plan.points = a.points > b.points ? a.points : b.points;
  plan.contours = a.contours > b.contours ? a.contours : b.contours;
  plan.outputBytes = a.outputBytes > b.outputBytes ? a.outputBytes : b.outputBytes;
  plan.poolBytes = a.poolBytes > b.poolBytes ? a.poolBytes : b.poolBytes;
  plan.maxPpem26_6 = a.maxPpem26_6 > b.maxPpem26_6 ? a.maxPpem26_6 : b.maxPpem26_6;
  return plan;
}

bool Rasterizer::allocate(const ScratchPlan& plan) {
  release();
  if (plan.points > kMaxScratchPoints || plan.contours > kMaxScratchContours || plan.outputBytes > kMaxOutputBytes ||
      plan.poolBytes < 2048 || plan.poolBytes > kPoolBytesSd || plan.maxPpem26_6 == 0 ||
      plan.maxPpem26_6 > kMaxPpem26_6) {
    LOG_ERR("TTF", "scratch plan out of range: %u points, %u contours, %lu output, %lu pool, %lu ppem", plan.points,
            plan.contours, static_cast<unsigned long>(plan.outputBytes), static_cast<unsigned long>(plan.poolBytes),
            static_cast<unsigned long>(plan.maxPpem26_6));
    return false;
  }
  // The pool comes first: the block's start is aligned for ftgrays' cells and for the points that follow.
  pointsOffset_ = alignUp(plan.poolBytes, 8);
  contoursOffset_ = pointsOffset_ + plan.points * static_cast<uint32_t>(sizeof(FT_Vector));
  tagsOffset_ = contoursOffset_ + plan.contours * static_cast<uint32_t>(sizeof(unsigned short));
  outputOffset_ = tagsOffset_ + plan.points;
  const uint32_t bytes = outputOffset_ + plan.outputBytes;
  // One block for the family's lifetime: a cell pool and outline arrays on the loop task's stack would take
  // 4-10 KB of its 16 KB, and per-glyph allocations would fragment the heap.
  block_ = makeUniqueNoThrow<uint8_t[]>(bytes);
  if (!block_) {
    LOG_ERR("TTF", "scratch block: %lu bytes, out of memory", static_cast<unsigned long>(bytes));
    return false;
  }
  blockBytes_ = bytes;
  plan_ = plan;
  loaded_ = Status::Transient;
  return true;
}

void Rasterizer::release() {
  block_.reset();
  blockBytes_ = 0;
  plan_ = ScratchPlan{};
  loaded_ = Status::Transient;
}

Status Rasterizer::load(FontBytes& bytes, const Face& face, const uint16_t glyph, const uint32_t ppem26_6,
                        EpdGlyph* metrics) {
  *metrics = EpdGlyph{};
  numPoints_ = numContours_ = width_ = height_ = 0;
  if (!block_) return loaded_ = Status::Transient;
  if (ppem26_6 == 0 || ppem26_6 > kMaxPpem26_6) return loaded_ = Status::Unsupported;

  bytes.clearErrors();
  auto* points = reinterpret_cast<FT_Vector*>(block_.get() + pointsOffset_);
  Loader loader{bytes,
                face,
                unitsToPixels(face, ppem26_6),
                points,
                reinterpret_cast<unsigned short*>(block_.get() + contoursOffset_),
                block_.get() + tagsOffset_,
                plan_.points,
                plan_.contours};
  int32_t pp1x = 0;
  Status status = loader.load(glyph, 0, &pp1x);
  if (bytes.ioError()) status = Status::Transient;
  if (status == Status::Ok && bytes.outOfRange()) status = Status::Malformed;
  if (status == Status::Ok && !loader.translate(0, -pp1x, 0)) status = Status::Unsupported;
  if (status != Status::Ok) return loaded_ = status;
  const uint32_t advance = advance12_4(bytes, face, glyph, ppem26_6);
  if (bytes.ioError()) return loaded_ = Status::Transient;
  if (advance > 0xFFFF) return loaded_ = Status::Unsupported;  // past the 4,095.9 px EpdGlyph::advanceX holds

  int32_t left = 0;
  int32_t top = 0;
  int32_t width = 0;
  int32_t height = 0;
  if (loader.numPoints > 0) {
    // FreeType's bitmap box for gray rendering: the control box, floored and ceiled to whole pixels.
    FT_Pos xMin = points[0].x;
    FT_Pos yMin = points[0].y;
    FT_Pos xMax = xMin;
    FT_Pos yMax = yMin;
    for (uint16_t i = 1; i < loader.numPoints; ++i) {
      xMin = points[i].x < xMin ? points[i].x : xMin;
      yMin = points[i].y < yMin ? points[i].y : yMin;
      xMax = points[i].x > xMax ? points[i].x : xMax;
      yMax = points[i].y > yMax ? points[i].y : yMax;
    }
    left = static_cast<int32_t>(xMin >> 6);
    const int32_t bottom = static_cast<int32_t>(yMin >> 6);
    top = static_cast<int32_t>((yMax + 63) >> 6);
    width = static_cast<int32_t>((xMax + 63) >> 6) - left;
    height = top - bottom;
    if (width > 255 || height > 255 || left < INT16_MIN || left > INT16_MAX || top < INT16_MIN || top > INT16_MAX) {
      return loaded_ = Status::Unsupported;
    }
    if (glyphDataBytes(width, height, true) > plan_.outputBytes) {
      // Past the output region: the glyph's own limit at a size the plan was made for; above those sizes the
      // caller must plan again, and nothing is wrong with the glyph.
      return loaded_ = ppem26_6 > plan_.maxPpem26_6 ? Status::Transient : Status::Unsupported;
    }
    loader.translate(0, -left * 64, -bottom * 64);  // the box now starts at (0, 0), as ftgrays' clip box does
  }
  metrics->width = static_cast<uint8_t>(width);
  metrics->height = static_cast<uint8_t>(height);
  metrics->advanceX = static_cast<uint16_t>(advance);
  metrics->left = static_cast<int16_t>(left);
  metrics->top = static_cast<int16_t>(top);
  metrics->dataLength = glyphDataBytes(width, height, true);
  numPoints_ = loader.numPoints;
  numContours_ = loader.numContours;
  width_ = static_cast<uint16_t>(width);
  height_ = static_cast<uint16_t>(height);
  return loaded_ = Status::Ok;
}

Status Rasterizer::rasterize(uint8_t* dst) {
  if (loaded_ != Status::Ok) return loaded_;
  const uint32_t bytes = glyphDataBytes(width_, height_, true);
  if (bytes == 0) return Status::Ok;
  std::memset(dst, 0, bytes);

  FT_Outline outline{};
  outline.n_contours = numContours_;
  outline.n_points = numPoints_;
  outline.points = reinterpret_cast<FT_Vector*>(block_.get() + pointsOffset_);
  outline.tags = block_.get() + tagsOffset_;
  outline.contours = reinterpret_cast<unsigned short*>(block_.get() + contoursOffset_);
  outline.flags = FT_OUTLINE_NONE;  // nonzero winding, as TrueType

  SpanSink sink{dst, width_, height_};
  FT_Raster_Params params{};
  params.source = &outline;
  params.flags = FT_RASTER_FLAG_AA | FT_RASTER_FLAG_DIRECT | FT_RASTER_FLAG_CLIP;
  params.gray_spans = packSpans;
  params.user = &sink;
  params.clip_box.xMax = width_;
  params.clip_box.yMax = height_;

  FT_Raster raster = nullptr;
  ttf_grays_raster.raster_new(nullptr, &raster);
  ttf_grays_raster.raster_reset(raster, block_.get(), plan_.poolBytes);
  return ttf_grays_raster.raster_render(raster, &params) == 0 ? Status::Ok : Status::Malformed;
}

Status Rasterizer::render(FontBytes& bytes, const Face& face, const uint16_t glyph, const uint32_t ppem26_6,
                          EpdGlyph* metrics) {
  const Status status = load(bytes, face, glyph, ppem26_6, metrics);
  return status == Status::Ok ? rasterize(output()) : status;
}

}  // namespace ttf
