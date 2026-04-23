#include "BidiUtils.h"

extern "C" {
#include "minibidi.h"
}

#undef when
#undef otherwise

#include <Utf8.h>

#include <cstring>

namespace {

void appendUtf8(uint32_t cp, std::string& out) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
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
    appendUtf8(line[i].wc, out);
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

  do_bidi(/*autodir=*/false, paragraphIsRtl ? 1 : 0, line, count);

  uint8_t seen[BIDI_MAX_LINE] = {0};
  int prevWord = -1;
  for (int i = 0; i < count; i++) {
    const uint16_t w = line[i].index;
    if (w >= nWords) continue;
    if (w != prevWord && !seen[w]) {
      visualOrder.push_back(w);
      seen[w] = 1;
    }
    prevWord = static_cast<int>(w);
  }

  for (size_t i = 0; i < nWords; i++) {
    if (!seen[i]) {
      visualOrder.push_back(static_cast<uint16_t>(i));
    }
  }

  if (visualOrder.size() != nWords) {
    visualOrder.clear();
    return false;
  }

  for (size_t i = 0; i < nWords; i++) {
    if (visualOrder[i] != i) {
      return true;
    }
  }

  visualOrder.clear();
  return false;
}

}  // namespace BidiUtils
