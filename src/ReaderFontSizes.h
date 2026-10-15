#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "CrossPointSettings.h"

/// The reader sizes one font family offers, in points, ascending and without repeats.
///
/// A stored reader size (CrossPointSettings::fontPointSize, a book's fontSizeOverride) need not be
/// one the active family offers. It may have been chosen under another family, or read from an
/// older settings file. Everything that shows, steps or draws a stored size goes through snap(),
/// so the settings row, the size buttons and the page cannot disagree. The stored value itself is
/// never rewritten to the snapped one, so switching back to the family it was chosen under brings
/// it back.
///
/// Fixed capacity and returned by value: the reader resolves its font through snap() while it lays
/// out pages, where nothing may allocate.
struct ReaderSizeList {
  // A family realistically ships at most 16 sizes -- the flash font partition caches no more than
  // that (FlashFontPartition::MAX_ENTRIES) -- plus the ladder sizes above its largest file.
  static constexpr uint8_t kCapacity = 24;
  uint8_t points[kCapacity] = {};
  uint8_t count = 0;

  /// What Bookerly and Noto Sans offer: the reader ladder, CrossPointSettings::FONT_SIZE_RUNGS.
  static ReaderSizeList builtin() {
    static_assert(CrossPointSettings::FONT_SIZE_RUNG_COUNT <= kCapacity, "the ladder must fit the list");
    ReaderSizeList list;
    for (const auto& rung : CrossPointSettings::FONT_SIZE_RUNGS) list.points[list.count++] = rung.points;
    return list;
  }

  /// The smallest size a natively drawn family offers. Points convert to pixels at the converter's 150 dpi on
  /// every board, so 8 pt is about 17 px per em on all of them; the denser the panel, the smaller it looks.
  static constexpr uint8_t kNativeSmallestPoints = 8;

  /// What a TrueType family offers: its glyphs are drawn from the outlines at any size, so the ladder plus the
  /// sizes below it, in the ladder's 2 pt steps, down to kNativeSmallestPoints.
  static ReaderSizeList native() {
    ReaderSizeList list = builtin();
    for (uint8_t pt = kNativeSmallestPoints; pt < list.points[0]; pt += 2) list.insert(pt);
    return list;
  }

  /// What an SD family offers: every size it ships a file for, then the ladder sizes above its
  /// largest file. Those are drawn from that file scaled up (SdCardFontManager::ensureSizeAlias),
  /// so large print stays available whatever the family was built at. A ladder size below the
  /// largest file is never offered: it would be drawn by shrinking a bigger face, and the family's
  /// own smaller files are the better answer. With no usable file sizes this is builtin().
  ///
  /// `familyPoints` in any order, repeats allowed: it is read straight off the directory listing.
  /// Sizes outside MIN/MAX_FONT_POINT_SIZE are skipped. Past kCapacity, the largest are dropped.
  static ReaderSizeList forFamily(const uint8_t* familyPoints, const size_t n) {
    ReaderSizeList list;
    for (size_t i = 0; i < n; ++i) list.insert(familyPoints[i]);
    const uint8_t largest = list.count > 0 ? list.points[list.count - 1] : 0;
    for (const auto& rung : CrossPointSettings::FONT_SIZE_RUNGS) {
      if (rung.points > largest) list.insert(rung.points);
    }
    return list;
  }

  /// Position of the size closest to `pt`. Ties go to the smaller, so the answer does not depend
  /// on which side of a gap a value sits.
  uint8_t indexOf(const uint8_t pt) const {
    uint8_t best = 0;
    int bestDiff = 256;
    for (uint8_t i = 0; i < count; ++i) {
      const int diff = points[i] > pt ? points[i] - pt : pt - points[i];
      if (diff < bestDiff) {
        best = i;
        bestDiff = diff;
      }
    }
    return best;
  }

  /// The size closest to `pt`; `pt` itself when it is offered.
  uint8_t snap(const uint8_t pt) const { return count == 0 ? pt : points[indexOf(pt)]; }

  /// `delta` sizes along the list from snap(pt), clamped at both ends rather than wrapped: a pinch
  /// that has reached the largest size should stay there.
  uint8_t step(const uint8_t pt, const int delta) const {
    if (count == 0) return pt;
    int target = indexOf(pt) + delta;
    if (target < 0) target = 0;
    if (target > count - 1) target = count - 1;
    return points[target];
  }

  /// The size after snap(pt), wrapping from the largest to the smallest (BTN_CYCLE_FONT_SIZE).
  uint8_t next(const uint8_t pt) const { return count == 0 ? pt : points[(indexOf(pt) + 1) % count]; }

 private:
  // Sorted insert that skips repeats and out-of-range sizes. When full, a size smaller than the
  // largest held displaces it, so the smallest kCapacity sizes are the ones kept.
  void insert(const uint8_t pt) {
    if (pt < CrossPointSettings::MIN_FONT_POINT_SIZE || pt > CrossPointSettings::MAX_FONT_POINT_SIZE) return;
    uint8_t at = 0;
    while (at < count && points[at] < pt) ++at;
    if (at < count && points[at] == pt) return;
    if (count == kCapacity) {
      if (at == kCapacity) return;
      --count;
    }
    for (uint8_t i = count; i > at; --i) points[i] = points[i - 1];
    points[at] = pt;
    ++count;
  }
};

/// What a size option displays: the point size itself, "14pt". Untranslated: the numeral carries
/// the meaning and "pt" is the unit in every locale this ships with.
inline std::string fontPointSizeLabel(const uint8_t pt) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%upt", static_cast<unsigned>(pt));
  return buf;
}
