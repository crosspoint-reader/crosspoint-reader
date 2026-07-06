#include "BidiUtils.h"

extern "C" {
#include "minibidi.h"
}

#undef when
#undef otherwise

#include <Logging.h>
#include <Utf8.h>

#include <cstring>
#include <mutex>

// Guards the static bidi_char buffers in applyBidiVisual() and
// computeVisualWordOrder().  The bidi+shaping pipeline is not reentrant;
// this mutex serialises access so multi-core callers don't corrupt each
// other's intermediate state.
static std::mutex bidiMutex;

namespace {

bool isNaturalDirectionClass(const uchar cls) {
  switch (cls) {
    case L:
    case R:
    case AL:
    case EN:
    case AN:
      return true;
    default:
      return false;
  }
}

}  // namespace

namespace BidiUtils {

bool startsWithRtl(const char* utf8, int maxStrongChars) {
  if (!utf8 || maxStrongChars <= 0) return false;

  auto* p = reinterpret_cast<const unsigned char*>(utf8);
  int checked = 0;
  while (*p) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (!cp || cp == REPLACEMENT_GLYPH) break;

    const uchar cls = bidi_class(cp);
    if (cls == R || cls == AL) return true;
    if (cls == L) return false;
    checked++;
    if (checked >= maxStrongChars) break;
  }
  return false;
}

int detectParagraphLevel(const char* utf8, const int fallbackLevel, const int maxStrongChars) {
  if (!utf8 || maxStrongChars <= 0) return fallbackLevel & 1;

  auto* p = reinterpret_cast<const unsigned char*>(utf8);
  int checked = 0;
  while (*p) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (!cp || cp == REPLACEMENT_GLYPH) break;

    const uchar cls = bidi_class(cp);
    if (cls == R || cls == AL) return 1;
    if (cls == L) return 0;
    checked++;
    if (checked >= maxStrongChars) break;
  }

  return fallbackLevel & 1;
}

bool isTransparentMark(const uint32_t cp) {
  // RTL-script combining marks: Hebrew niqqud/cantillation and Arabic
  // harakat/Quranic annotation.  Transparent for Arabic joining (do_shape
  // skips them), zero-advance for measurement, and rendered as overlays on
  // the preceding base glyph when the active font carries their glyphs.
  // The cp >= 0x0591 guard keeps Latin combining marks (U+0300-U+036F, also
  // NSM) on their existing utf8IsCombiningMark() rendering path.
  return cp >= 0x0591 && bidi_class(cp) == NSM;
}

bool applyBidiVisual(const char* utf8, std::string& out, int paragraphLevel) {
  if (!utf8 || !*utf8) return false;
  const std::lock_guard<std::mutex> lock(bidiMutex);

  static bidi_char line[BIDI_MAX_LINE];
  static bidi_char shaped[BIDI_MAX_LINE];
  int count = 0;
  int lastBase = -1;           // last non-formatter character (mintty's ibase)
  uint8_t pendingJoiners = 0;  // ZWJ/ZWNJ seen since lastBase
  auto* p = reinterpret_cast<const unsigned char*>(utf8);
  while (*p) {
    if (count >= BIDI_MAX_LINE) {
      LOG_DBG("BIDI", "applyBidiVisual: input exceeds BIDI_MAX_LINE (%d chars), returning unprocessed", BIDI_MAX_LINE);
      return false;
    }

    const uint32_t cp = utf8NextCodepoint(&p);
    if (!cp || cp == REPLACEMENT_GLYPH) break;
    line[count].origwc = line[count].wc = cp;
    line[count].index = static_cast<uint16_t>(count);
    line[count].joiners = 0;

    // Flag Arabic joining formatters mintty-style (termline.c): the ZWJ/ZWNJ
    // goes into the low nibble of the character it follows and the high
    // nibble of the character it precedes.  Flags are assigned in logical
    // order here; do_shape() reads them after reordering.
    if (cp == 0x200C || cp == 0x200D) {
      const uint8_t joiner = (cp == 0x200D) ? ZWJ : ZWNJ;
      if (lastBase >= 0) line[lastBase].joiners |= joiner;
      pendingJoiners |= joiner;
    } else {
      line[count].joiners = pendingJoiners << 4;
      pendingJoiners = 0;
      lastBase = count;
    }
    count++;
  }
  if (!count) return false;

  const bool autodir = (paragraphLevel < 0);
  const int level = autodir ? 0 : (paragraphLevel & 1);

  // Order matters (mintty does the same): do_bidi() first to obtain visual
  // order, then do_shape() — contextual forms are resolved from *visual*
  // adjacency, and shaping presentation forms must never be reordered.
  do_bidi(autodir, level, line, count);
  do_shape(line, shaped, count);

  out.clear();
  out.reserve(std::strlen(utf8));
  // Lam-Alef collapse sentinel and zero-width joining formatters have done
  // their job during shaping and have no glyphs to render.
  const auto filtered = [](const uint32_t cp) { return cp == LIGATURE_PLACEHOLDER || cp == 0x200C || cp == 0x200D; };
  for (int i = 0; i < count; i++) {
    const uint32_t cp = shaped[i].wc;
    if (filtered(cp)) continue;
    if (!isTransparentMark(cp)) {
      utf8AppendCodepoint(cp, out);
      continue;
    }
    // UAX#9 rule L3: reversing an RTL run leaves combining marks *before*
    // their base character. The renderer overlays a mark on the most
    // recently drawn glyph, so emit the base first, then its marks in
    // logical order. `index` is the original logical position: a base
    // following its marks with a *lower* index means the run was reversed.
    int j = i;  // [i, j) = the run of marks (and filtered entries)
    while (j < count && (filtered(shaped[j].wc) || isTransparentMark(shaped[j].wc))) j++;
    if (j < count && shaped[j].index < shaped[i].index) {
      utf8AppendCodepoint(shaped[j].wc, out);
      for (int k = j - 1; k >= i; k--) {
        if (isTransparentMark(shaped[k].wc)) utf8AppendCodepoint(shaped[k].wc, out);
      }
      i = j;  // base already emitted
    } else {
      // Unreversed (or trailing, base-less) marks already follow their base.
      for (int k = i; k < j; k++) {
        if (isTransparentMark(shaped[k].wc)) utf8AppendCodepoint(shaped[k].wc, out);
      }
      i = j - 1;
    }
  }
  return true;
}

bool computeVisualWordOrder(const std::vector<std::string>& words, bool paragraphIsRtl,
                            std::vector<uint16_t>& visualOrder) {
  visualOrder.clear();
  const size_t nWords = words.size();
  if (nWords <= 1 || nWords > BIDI_MAX_LINE) return false;
  const std::lock_guard<std::mutex> lock(bidiMutex);

  static bidi_char line[BIDI_MAX_LINE];
  int count = 0;
  bool truncated = false;

  for (size_t w = 0; w < nWords && !truncated; w++) {
    auto* p = reinterpret_cast<const unsigned char*>(words[w].c_str());
    while (*p) {
      if (count >= BIDI_MAX_LINE) {
        truncated = true;
        break;
      }
      const uint32_t cp = utf8NextCodepoint(&p);
      if (!cp || cp == REPLACEMENT_GLYPH) break;
      line[count].origwc = line[count].wc = cp;
      line[count].index = static_cast<uint16_t>(w);
      line[count].joiners = 0;
      count++;
    }

    if (!truncated && w + 1 < nWords) {
      if (count >= BIDI_MAX_LINE) {
        truncated = true;
        break;
      }
      line[count].origwc = line[count].wc = ' ';
      line[count].index = static_cast<uint16_t>(nWords);
      line[count].joiners = 0;
      count++;
    }
  }

  if (truncated || count == 0) return false;

  // Fast-path for homogeneous lines: skip UAX#9 if there's no mixing.
  bool hasL = false, hasR = false;
  for (int i = 0; i < count; i++) {
    uchar bc = bidi_class(line[i].wc);
    if (bc == L || bc == EN || bc == AN)
      hasL = true;
    else if (bc == R || bc == AL)
      hasR = true;
  }

  // Purely LTR line in RTL paragraph: identity order, but we might still need to reorder
  // if some characters are mirrored or neutral resolution differs.
  // Actually, UAX#9 rule L1/L2 says purely LTR in RTL para stays as is (identity).
  // Purely RTL line: just reverse the words.
  if (!hasL && hasR && paragraphIsRtl) {
    visualOrder.reserve(nWords);
    for (int i = static_cast<int>(nWords) - 1; i >= 0; i--) {
      visualOrder.push_back(static_cast<uint16_t>(i));
    }
    return true;
  }
  if (!hasR) {
    if (!paragraphIsRtl) {
      // Pure LTR in LTR paragraph: nothing to do.
      return false;
    }
    // Pure LTR in RTL paragraph: no word reordering, but must use the
    // willReorder (left-to-right) positioning path, not the RTL right-to-left path.
    visualOrder.reserve(nWords);
    for (size_t i = 0; i < nWords; i++) {
      visualOrder.push_back(static_cast<uint16_t>(i));
    }
    return true;
  }

  do_bidi(/*autodir=*/false, paragraphIsRtl ? 1 : 0, line, count);

  uint16_t firstAny[BIDI_MAX_LINE];
  uint16_t firstNatural[BIDI_MAX_LINE];
  for (size_t w = 0; w < nWords; w++) {
    firstAny[w] = UINT16_MAX;
    firstNatural[w] = UINT16_MAX;
  }

  for (int i = 0; i < count; i++) {
    const uint16_t w = line[i].index;
    if (w >= nWords) continue;

    if (firstAny[w] == UINT16_MAX) {
      firstAny[w] = static_cast<uint16_t>(i);
    }

    if (firstNatural[w] == UINT16_MAX && isNaturalDirectionClass(bidi_class(line[i].wc))) {
      firstNatural[w] = static_cast<uint16_t>(i);
    }
  }

  visualOrder.reserve(nWords);
  for (int i = 0; i < count; i++) {
    const uint16_t w = line[i].index;
    if (w >= nWords) continue;

    const uint16_t anchor = firstNatural[w] != UINT16_MAX ? firstNatural[w] : firstAny[w];
    if (anchor == UINT16_MAX) {
      visualOrder.clear();
      return false;
    }
    if (anchor == static_cast<uint16_t>(i)) {
      visualOrder.push_back(w);
    }
  }

  if (visualOrder.size() != nWords) {
    visualOrder.clear();
    return false;
  }

  // Check if the order is exactly the same as the original input
  bool needsReorder = false;
  for (size_t i = 0; i < nWords; i++) {
    if (visualOrder[i] != i) {
      needsReorder = true;
      break;
    }
  }

  if (!needsReorder) {
    visualOrder.clear();
    return false;
  }

  return true;
}

}  // namespace BidiUtils
