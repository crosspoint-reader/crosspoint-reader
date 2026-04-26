#include "BidiUtils.h"

extern "C" {
#include "minibidi.h"
}

#undef when
#undef otherwise

#include <Utf8.h>

#include <cstring>

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
    if (cls == EN || cls == AN) return 0;
    checked++;
    if (checked >= maxStrongChars) break;
  }

  return fallbackLevel & 1;
}

std::string applyBidiVisual(const char* utf8, int paragraphLevel) {
  if (!utf8 || !*utf8) return {};

  static bidi_char line[BIDI_MAX_LINE];
  int count = 0;
  auto* p = reinterpret_cast<const unsigned char*>(utf8);
  while (*p) {
    if (count >= BIDI_MAX_LINE) {
      // Keep original text when the input is longer than the mini-bidi working buffer.
      return utf8;
    }

    const uint32_t cp = utf8NextCodepoint(&p);
    if (!cp || cp == REPLACEMENT_GLYPH) break;
    line[count].origwc = line[count].wc = cp;
    line[count].index = static_cast<uint16_t>(count);
    count++;
  }
  if (!count) return {};

  const bool autodir = (paragraphLevel < 0);
  const int level = autodir ? 0 : (paragraphLevel & 1);
  do_bidi(autodir, level, line, count);

  std::string out;
  out.reserve(std::strlen(utf8));
  for (int i = 0; i < count; i++) {
    utf8AppendCodepoint(line[i].wc, out);
  }
  return out;
}

bool computeVisualWordOrder(const std::vector<std::string>& words, bool paragraphIsRtl,
                            std::vector<uint16_t>& visualOrder) {
  visualOrder.clear();
  const size_t nWords = words.size();
  if (nWords <= 1 || nWords > BIDI_MAX_LINE) return false;

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
      count++;
    }

    if (!truncated && w + 1 < nWords) {
      if (count >= BIDI_MAX_LINE) {
        truncated = true;
        break;
      }
      line[count].origwc = line[count].wc = ' ';
      line[count].index = static_cast<uint16_t>(nWords);
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
