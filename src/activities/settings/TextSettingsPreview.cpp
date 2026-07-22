#include "TextSettingsPreview.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "fontIds.h"

namespace {
constexpr const char* ELLIPSIS_UTF8 = "\xe2\x80\xa6";

// Bold-prefix byte length for one focus-reading word: the first
// clamp(45% of chars, 1, 9) UTF-8 characters. Shared by the measure and draw
// passes so they can't disagree.
size_t focusBoldBytes(const char* wordStart, size_t wordBytes) {
  size_t charCount = 0;
  for (size_t i = 0; i < wordBytes; i++) {
    if ((wordStart[i] & 0xC0) != 0x80) charCount++;  // UTF-8 lead bytes
  }
  const size_t boldChars = std::clamp<size_t>(charCount * 45 / 100, 1, 9);
  if (boldChars >= charCount) return wordBytes;
  size_t chars = 0;
  for (size_t i = 0; i < wordBytes; i++) {
    if ((wordStart[i] & 0xC0) != 0x80) {
      if (chars == boldChars) return i;
      chars++;
    }
  }
  return wordBytes;
}

// Natural rendered width, mirroring drawLine's per-word advance so
// alignment/justify math matches the drawn glyphs.
int measureLineWidth(const GfxRenderer& renderer, int fontId, const char* line, bool focusReading) {
  const int spaceW = renderer.getSpaceWidth(fontId);
  const char* p = line;
  int w = 0;
  char buf[128];
  while (*p) {
    if (*p == ' ') {
      w += spaceW;
      p++;
      continue;
    }
    const char* wordStart = p;
    while (*p && *p != ' ') p++;
    const size_t wordBytes = p - wordStart;
    const size_t boldBytes = focusReading ? focusBoldBytes(wordStart, wordBytes) : 0;
    if (boldBytes > 0) {
      snprintf(buf, sizeof(buf), "%.*s", static_cast<int>(boldBytes), wordStart);
      w += renderer.getTextAdvanceX(fontId, buf, EpdFontFamily::BOLD);
    }
    if (boldBytes < wordBytes) {
      snprintf(buf, sizeof(buf), "%.*s", static_cast<int>(wordBytes - boldBytes), wordStart + boldBytes);
      w += renderer.getTextAdvanceX(fontId, buf, EpdFontFamily::REGULAR);
    }
  }
  return w;
}

// Draws a line word by word: adds extraSpace to each gap (justification) and
// applies the focus-reading bold split per word.
void drawLine(const GfxRenderer& renderer, int fontId, int x, int y, const char* line, bool focusReading,
              int extraSpace) {
  const int gap = renderer.getSpaceWidth(fontId) + extraSpace;
  const char* p = line;
  int penX = x;
  char buf[128];
  while (*p) {
    if (*p == ' ') {
      penX += gap;
      p++;
      continue;
    }
    const char* wordStart = p;
    while (*p && *p != ' ') p++;
    const size_t wordBytes = p - wordStart;
    const size_t boldBytes = focusReading ? focusBoldBytes(wordStart, wordBytes) : 0;

    if (boldBytes > 0) {
      snprintf(buf, sizeof(buf), "%.*s", static_cast<int>(boldBytes), wordStart);
      renderer.drawText(fontId, penX, y, buf, true, EpdFontFamily::BOLD);
      penX += renderer.getTextAdvanceX(fontId, buf, EpdFontFamily::BOLD);
    }
    if (boldBytes < wordBytes) {
      snprintf(buf, sizeof(buf), "%.*s", static_cast<int>(wordBytes - boldBytes), wordStart + boldBytes);
      renderer.drawText(fontId, penX, y, buf, true, EpdFontFamily::REGULAR);
      penX += renderer.getTextAdvanceX(fontId, buf, EpdFontFamily::REGULAR);
    }
  }
}
}  // namespace

namespace textsettings {

void renderPreview(GfxRenderer& renderer, int previewPadding, int labelGap, int top, int height, const char* familyName,
                   const char* sizeName) {
  const int left = previewPadding;
  const int width = renderer.getScreenWidth() - (previewPadding * 2);
  if (width <= 0 || height <= 0) return;

  const int labelH = renderer.getTextHeight(UI_10_FONT_ID);
  const int labelReserved = labelH + labelGap + previewPadding;

  char labelBuf[128];
  snprintf(labelBuf, sizeof(labelBuf), "%s \"%s, %s\"", tr(STR_PREVIEW), familyName, sizeName);
  const int labelY = top + height - previewPadding - labelH;
  renderer.drawText(UI_10_FONT_ID, left, labelY, labelBuf);

  const int fontId = SETTINGS.getReaderFontId();
  if (fontId == 0) return;

  const int lineH = renderer.getTextHeight(fontId);
  if (lineH <= 0) return;

  const int textLeft = left + SETTINGS.screenMargin;
  const int textWidth = width - 2 * SETTINGS.screenMargin;
  if (textWidth <= 0) return;

  const int lineAdvance = std::max(1, renderer.getLineHeight(fontId, SETTINGS.getReaderLineCompression()));
  const int paragraphGap = SETTINGS.extraParagraphSpacing ? lineAdvance / 2 : 0;

  const int innerHeight = height - previewPadding - labelReserved;
  const int maxLines = std::max(1, innerHeight / lineAdvance);

  const char* previewText = I18N.get(StrId::STR_FONT_PREVIEW_TEXT);
  const char* extText = I18N.get(StrId::STR_FONT_PREVIEW_TEXT_EXT);  // second paragraph, for the paragraph gap
  const bool focusReading = SETTINGS.focusReadingEnabled != 0;
  if (auto* fcm = renderer.getFontCacheManager()) {
    char prewarmBuf[256];
    snprintf(prewarmBuf, sizeof(prewarmBuf), "%s %s %s", previewText, extText, ELLIPSIS_UTF8);
    fcm->prewarmCache(fontId, prewarmBuf, focusReading ? 0x03 : 0x01);  // 0x03 also warms bold
  }

  // Focus-reading bold prefixes widen each word; wrap narrower so they fit
  const int wrapWidth = focusReading ? textWidth - textWidth / 12 : textWidth;
  const auto lines = renderer.wrappedText(fontId, previewText, wrapWidth, maxLines);
  const auto extLines = renderer.wrappedText(fontId, extText, wrapWidth, maxLines);

  const uint8_t align = SETTINGS.paragraphAlignment;
  const bool justify = align == CrossPointSettings::JUSTIFIED || align == CrossPointSettings::BOOK_STYLE;
  auto lineStartX = [&](int naturalW) -> int {
    if (align == CrossPointSettings::CENTER_ALIGN) return textLeft + std::max(0, (textWidth - naturalW) / 2);
    if (align == CrossPointSettings::RIGHT_ALIGN) return textLeft + std::max(0, textWidth - naturalW);
    return textLeft;
  };

  int y = top + previewPadding;
  const int textBottomLimit = top + height - labelReserved;
  const std::vector<std::string>* paragraphs[2] = {&lines, extLines.empty() ? &lines : &extLines};
  for (const auto* paragraph : paragraphs) {
    const size_t lastLine = paragraph->empty() ? 0 : paragraph->size() - 1;
    for (size_t i = 0; i < paragraph->size(); i++) {
      if (y + lineH > textBottomLimit) break;
      const std::string& line = (*paragraph)[i];
      const int naturalW = measureLineWidth(renderer, fontId, line.c_str(), focusReading);

      int extraSpace = 0;
      if (justify && i != lastLine) {
        const int gaps = static_cast<int>(std::count(line.begin(), line.end(), ' '));
        if (gaps > 0) extraSpace = std::max(0, textWidth - naturalW) / gaps;
      }

      drawLine(renderer, fontId, lineStartX(naturalW), y, line.c_str(), focusReading, extraSpace);
      y += lineAdvance;
    }
    y += paragraphGap;
  }
}

}  // namespace textsettings
