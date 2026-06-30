#include "FocusReading.h"

#include <GfxRenderer.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>

namespace FocusReading {

bool isWordCharacter(uint32_t cp) {
  // ASCII range (Catches 95%+ of characters immediately)
  if (cp < 128) {
    // Bitwise trick: (cp | 0x20) converts uppercase ASCII to lowercase.
    // This checks for A-Z and a-z mathematically, avoiding memory lookups and <cctype>
    return ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z') || cp == '\'';
  }

  // General Punctuation Block, Currency, Math, Arrows, & Symbols (0x2000 - 0x2BFF)
  if (cp >= 0x2000 && cp <= 0x2BFF) {
    // Explicitly allow smart quotes, reject all other general punctuation (em-dashes, etc.)
    return cp == 0x2018 || cp == 0x2019;
  }

  // Latin-1 Punctuation Block (0x00A1 - 0x00BF)
  if (cp >= 0x00A1 && cp <= 0x00BF) {
    // Allow ordinal indicators and micro sign, reject the rest (¡, ¿, «, », etc.)
    return cp == 0x00AA || cp == 0x00B5 || cp == 0x00BA;
  }

  // Rejects Two-em dash, Three-em dash, Double oblique hyphen, etc.
  if (cp >= 0x2E00 && cp <= 0x2E7F) return false;
  // Rejects Modifier Minus (0x02D7), Small Hyphen (0xFE63), and Fullwidth Hyphen (0xFF0D)
  if (cp == 0x02D7 || cp == 0xFE63 || cp == 0xFF0D) return false;
  // Assume all other Unicode ranges (accented letters, Cyrillic, Greek, etc.) are valid

  return true;
}

SplitInfo computeSplitInfo(std::string_view word) {
  // Target 45% for 1-bold at 4 chars and 3-bold at 7 chars with floor truncation
  static constexpr size_t FOCUS_READING_PERCENT = 45;

  size_t charCount = 0;
  const unsigned char* countPtr = reinterpret_cast<const unsigned char*>(word.data());
  const unsigned char* const countEnd = countPtr + word.size();
  while (countPtr < countEnd) {
    utf8NextCodepoint(&countPtr);
    charCount++;
  }

  if (charCount == 0) return {};

  size_t targetBoldChars = (charCount * FOCUS_READING_PERCENT) / 100;
  targetBoldChars = std::clamp<size_t>(targetBoldChars, 1, 9);
  if (targetBoldChars >= charCount) {
    return SplitInfo{static_cast<uint8_t>(word.size()), true};
  }

  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(word.data());
  for (size_t i = 0; i < targetBoldChars; ++i) {
    utf8NextCodepoint(&ptr);
  }
  return SplitInfo{static_cast<uint8_t>(ptr - reinterpret_cast<const unsigned char*>(word.data())), false};
}

}  // namespace FocusReading

namespace {

bool shouldApplyFocus(bool focusReadingEnabled, EpdFontFamily::Style style) {
  return focusReadingEnabled && (style & EpdFontFamily::BOLD) == 0;
}

int drawMeasuredSegment(const GfxRenderer& renderer, int fontId, int x, int y, std::string_view segment, bool black,
                        EpdFontFamily::Style style, bool focusReadingEnabled, BidiUtils::BidiBaseDir baseDir,
                        bool draw) {
  if (segment.empty()) return 0;

  std::string segmentText(segment);
  if (!shouldApplyFocus(focusReadingEnabled, style)) {
    if (draw) renderer.drawText(fontId, x, y, segmentText.c_str(), black, style, baseDir);
    return renderer.getTextAdvanceX(fontId, segmentText.c_str(), style);
  }

  const auto split = FocusReading::computeSplitInfo(segment);
  const auto boldStyle = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::BOLD);
  if (split.wholeBold) {
    if (draw) renderer.drawText(fontId, x, y, segmentText.c_str(), black, boldStyle, baseDir);
    return renderer.getTextAdvanceX(fontId, segmentText.c_str(), boldStyle);
  }

  std::string prefix(segment.substr(0, split.boundaryBytes));
  const char* suffix = segmentText.c_str() + split.boundaryBytes;
  const int prefixAdvance = renderer.getTextAdvanceX(fontId, prefix.c_str(), boldStyle);
  if (draw) {
    renderer.drawText(fontId, x, y, prefix.c_str(), black, boldStyle, baseDir);
    renderer.drawText(fontId, x + prefixAdvance, y, suffix, black, style, baseDir);
  }
  return prefixAdvance + renderer.getTextAdvanceX(fontId, suffix, style);
}

int drawMeasuredAnnotatedWord(const GfxRenderer& renderer, int fontId, int x, int y, const char* text, bool black,
                              EpdFontFamily::Style style, const FocusReading::Annotation& annotation,
                              BidiUtils::BidiBaseDir baseDir, bool draw) {
  if (!text || !*text) return 0;
  if (annotation.boundary == 0) {
    if (draw) renderer.drawText(fontId, x, y, text, black, style, baseDir);
    return renderer.getTextAdvanceX(fontId, text, style);
  }

  const size_t textLen = strlen(text);
  const size_t boundary = std::min<size_t>(annotation.boundary, textLen);
  if (boundary >= textLen) {
    const auto boldStyle = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::BOLD);
    if (draw) renderer.drawText(fontId, x, y, text, black, boldStyle, baseDir);
    return renderer.getTextAdvanceX(fontId, text, boldStyle);
  }

  std::string prefix(text, boundary);
  const char* suffix = text + boundary;
  const auto boldStyle = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::BOLD);
  if (draw) {
    renderer.drawText(fontId, x, y, prefix.c_str(), black, boldStyle, baseDir);
    renderer.drawText(fontId, x + annotation.suffixX, y, suffix, black, style, baseDir);
  }
  return annotation.suffixX + renderer.getTextAdvanceX(fontId, suffix, style);
}

template <typename SegmentFn>
void forEachSegment(const char* text, SegmentFn&& segmentFn) {
  if (!text || !*text) return;

  const unsigned char* segmentStart = reinterpret_cast<const unsigned char*>(text);
  const unsigned char* p = segmentStart;
  const unsigned char* const end = p + strlen(text);
  bool inWord = false;
  bool haveType = false;

  while (p < end) {
    const unsigned char* cpStart = p;
    const uint32_t cp = utf8NextCodepoint(&p);
    const bool isWord = FocusReading::isWordCharacter(cp);
    if (!haveType) {
      inWord = isWord;
      haveType = true;
      continue;
    }
    if (isWord != inWord) {
      segmentFn(reinterpret_cast<const char*>(segmentStart), static_cast<size_t>(cpStart - segmentStart), inWord);
      segmentStart = cpStart;
      inWord = isWord;
    }
  }

  if (haveType && segmentStart < end) {
    segmentFn(reinterpret_cast<const char*>(segmentStart), static_cast<size_t>(end - segmentStart), inWord);
  }
}

}  // namespace

namespace FocusReading {

int getTextAdvanceX(const GfxRenderer& renderer, int fontId, const char* text, EpdFontFamily::Style style,
                    bool focusReadingEnabled, const Annotation* annotation, BidiUtils::BidiBaseDir baseDir) {
  if (!text || !*text) return 0;
  if (annotation && annotation->boundary > 0) {
    return drawMeasuredAnnotatedWord(renderer, fontId, 0, 0, text, true, style, *annotation, baseDir, false);
  }
  if (!focusReadingEnabled) {
    return renderer.getTextAdvanceX(fontId, text, style);
  }

  int advance = 0;
  forEachSegment(text, [&](const char* segmentStart, size_t segmentLen, bool isWord) {
    advance += drawMeasuredSegment(renderer, fontId, 0, 0, std::string_view(segmentStart, segmentLen), true, style,
                                   isWord, baseDir, false);
  });
  return advance;
}

void drawText(const GfxRenderer& renderer, int fontId, int x, int y, const char* text, bool black,
              EpdFontFamily::Style style, bool focusReadingEnabled, const Annotation* annotation,
              BidiUtils::BidiBaseDir baseDir) {
  if (!text || !*text) return;
  if (annotation && annotation->boundary > 0) {
    drawMeasuredAnnotatedWord(renderer, fontId, x, y, text, black, style, *annotation, baseDir, true);
    return;
  }
  if (!focusReadingEnabled) {
    renderer.drawText(fontId, x, y, text, black, style, baseDir);
    return;
  }

  int cursorX = x;
  forEachSegment(text, [&](const char* segmentStart, size_t segmentLen, bool isWord) {
    cursorX += drawMeasuredSegment(renderer, fontId, cursorX, y, std::string_view(segmentStart, segmentLen), black,
                                   style, isWord, baseDir, true);
  });
}

}  // namespace FocusReading
