#include "TextPopupActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HapticFeedback.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "components/UITheme.h"

namespace {

constexpr int FRAME_INNER_PAD = 10;      // text inset inside the frame
constexpr int FRAME_LINE_WIDTH = 2;      // frame border thickness
constexpr int FRAME_MAX_HEIGHT_PCT = 62; // frame height cap, % of screen height
constexpr int HEADER_LINES = 1;          // title strip consumes one text line
constexpr int MAX_LINE_BYTES = 120;
constexpr float LINE_SPACING = 1.25f;

}  // namespace

TextPopupActivity::TextPopupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     std::unique_ptr<Page> page, const int marginLeft, const int marginTop,
                                     std::string title, std::string text, const char* activityName)
    : Activity(activityName, renderer, mappedInput),
      page(std::move(page)),
      marginLeft(marginLeft),
      marginTop(marginTop),
      title(std::move(title)),
      text(std::move(text)) {}

void TextPopupActivity::onEnter() {
  Activity::onEnter();
  fontId = SETTINGS.getReaderFontId();
  // Direct-use activities (footnote popup) hand their text to the constructor:
  // lay it out now. Subclasses that produce text later (dictionary) start
  // empty and call showText() once their payload is ready.
  if (!text.empty() && lines.empty()) {
    layoutFrame();
    if (!wrapText()) {
      text.clear();
      text.shrink_to_fit();
    }
  }
  // The first frame never paints without this: nothing else requests an
  // update for a direct-use popup (the dictionary path asks from its own
  // lookup flow), so the screen would wait for the next input event.
  requestUpdate();
}

void TextPopupActivity::layoutFrame() {
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  frameW = screenWidth - 2 * marginLeft;
  frameH = (screenHeight * FRAME_MAX_HEIGHT_PCT) / 100;
  frameX = marginLeft;
  frameY = (screenHeight - frameH) / 3;
}

bool TextPopupActivity::wrapText() {
  const int textWidth = frameW - 2 * FRAME_INNER_PAD;
  const int lineHeight = renderer.getLineHeight(fontId, LINE_SPACING);
  if (textWidth <= 0 || lineHeight <= 0) {
    LOG_ERR("TPOP", "geometry reject: frameW=%d textWidth=%d lineHeight=%d fontId=%d", frameW, textWidth, lineHeight,
            fontId);
    return false;
  }
  // Count the frame border against the text area: the last text line must stay
  // inside the border, or the page counter under-reports what the frame shows.
  linesPerPage = std::max(1, (frameH - 2 * FRAME_INNER_PAD - 2 * FRAME_LINE_WIDTH) / lineHeight - HEADER_LINES);

  // SD-card fonts: batch-load the glyphs up front (see
  // DictionaryDefinitionActivity::wrapText for the rationale); the title is a
  // separate string from the body, so warm both.
  renderer.ensureSdCardFontReady(fontId, title.c_str(), 0x03 /* REGULAR + BOLD */);
  renderer.ensureSdCardFontReady(fontId, text.c_str(), 0x03 /* REGULAR + BOLD */);

  const int spaceWidth = renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR);
  lines.reserve(text.size() / 24 + 4);
  scrollLine = 0;

  const uint32_t n = static_cast<uint32_t>(text.size());
  const char* bytes = text.c_str();
  uint32_t lineStart = 0;
  uint32_t lineEnd = 0;
  int lineWidth = 0;
  uint32_t i = 0;
  while (i < n) {
    const char c = bytes[i];
    if (c == '\n' || c == '\0') {
      lines.emplace_back(lineStart, static_cast<uint16_t>(lineEnd - lineStart));
      i++;
      lineStart = lineEnd = i;
      lineWidth = 0;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      i++;
      continue;
    }
    // Token: run of non-whitespace bytes, capped at the measure buffer.
    const uint32_t tokenStart = i;
    while (i < n && bytes[i] != ' ' && bytes[i] != '\t' && bytes[i] != '\r' && bytes[i] != '\n' && bytes[i] != '\0' &&
           i - tokenStart < MAX_LINE_BYTES) {
      i++;
    }
    // Back off a mid-UTF-8 cut to the last complete codepoint boundary.
    while (i - tokenStart > 1 && (bytes[i] & 0xC0) == 0x80) i--;
    const uint32_t tokenLen = i - tokenStart;
    char buf[MAX_LINE_BYTES + 1];
    memcpy(buf, bytes + tokenStart, tokenLen);
    buf[tokenLen] = '\0';
    const int tokenWidth = renderer.getTextAdvanceX(fontId, buf, EpdFontFamily::REGULAR);

    if (lineEnd == lineStart) {
      lineStart = tokenStart;
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    } else if (lineWidth + spaceWidth + tokenWidth <= textWidth &&
               tokenStart + tokenLen - lineStart <= UINT16_MAX) {
      lineEnd = tokenStart + tokenLen;
      lineWidth += spaceWidth + tokenWidth;
    } else {
      lines.emplace_back(lineStart, static_cast<uint16_t>(lineEnd - lineStart));
      lineStart = tokenStart;
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    }
  }
  if (lineEnd > lineStart) lines.emplace_back(lineStart, static_cast<uint16_t>(lineEnd - lineStart));
  if (lines.empty()) {
    LOG_ERR("TPOP", "wrap produced no lines: size=%u", (unsigned)text.size());
    return false;
  }
  return true;
}

bool TextPopupActivity::showText(std::string nextTitle, std::string nextText) {
  title = std::move(nextTitle);
  text = std::move(nextText);
  lines.clear();
  layoutFrame();
  if (!wrapText()) {
    text.clear();
    text.shrink_to_fit();
    lines.clear();
    return false;
  }
  return true;
}

void TextPopupActivity::scrollText(const int direction) {
  // Clamp, don't reject: the last page is usually shorter than linesPerPage,
  // so a full step would otherwise jump past it and leave it unreachable.
  const int maxScroll = std::max(0, static_cast<int>(lines.size()) - linesPerPage);
  const int next = std::clamp(scrollLine + direction, 0, maxScroll);
  if (next == scrollLine) return;
  scrollLine = next;
  requestUpdate();
}

void TextPopupActivity::drawPopupBody() {
  // Opaque panel: erase the frame area, then draw border, header, and text.
  renderer.fillRect(frameX, frameY, frameW, frameH, false);
  renderer.drawRect(frameX, frameY, frameW, frameH, FRAME_LINE_WIDTH, true);
  const int headerLineY = frameY + FRAME_LINE_WIDTH + renderer.getLineHeight(fontId, LINE_SPACING);
  renderer.drawLine(frameX, headerLineY, frameX + frameW, headerLineY, true);

  const int textX = frameX + FRAME_INNER_PAD;
  int textY = frameY + FRAME_LINE_WIDTH + FRAME_INNER_PAD;
  char headerBuf[64];
  const int headerLen = std::min<int>(static_cast<int>(title.size()), sizeof(headerBuf) - 1);
  memcpy(headerBuf, title.c_str(), headerLen);
  headerBuf[utf8SafeTruncateBuffer(headerBuf, headerLen)] = '\0';
  renderer.drawText(fontId, textX, textY, headerBuf, true, EpdFontFamily::BOLD);
  textY += renderer.getLineHeight(fontId, LINE_SPACING);

  const int firstLine = scrollLine;
  const int lastLine = std::min<int>(firstLine + linesPerPage, static_cast<int>(lines.size()));
  for (int row = firstLine; row < lastLine; row++) {
    const auto& [start, len] = lines[row];
    if (len == 0) continue;
    char buf[MAX_LINE_BYTES + 1];
    const size_t drawLen = std::min<size_t>(len, MAX_LINE_BYTES);
    memcpy(buf, text.c_str() + start, drawLen);
    buf[drawLen] = '\0';
    renderer.drawText(fontId, textX, textY, buf, true, EpdFontFamily::REGULAR);
    textY += renderer.getLineHeight(fontId, LINE_SPACING);
  }

  if (lines.size() > static_cast<size_t>(linesPerPage)) {
    // Scroll position indicator, bottom-right inside the frame. Round up: a
    // clamped scrollLine (short last page) still names the page it shows.
    const int current = (firstLine + linesPerPage - 1) / linesPerPage + 1;
    const int total = (static_cast<int>(lines.size()) + linesPerPage - 1) / linesPerPage;
    char counter[12];
    snprintf(counter, sizeof(counter), "%d/%d", current, total);
    const int counterW = renderer.getTextWidth(fontId, counter, EpdFontFamily::REGULAR);
    renderer.drawText(fontId, frameX + frameW - FRAME_INNER_PAD - counterW,
                      frameY + frameH - FRAME_LINE_WIDTH - renderer.getLineHeight(fontId, LINE_SPACING) + 2, counter,
                      true, EpdFontFamily::REGULAR);
  }
}

void TextPopupActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Tap inside the frame scrolls (left third of the frame = back a step,
  // the rest = forward); a tap outside the frame dismisses the popup.
  int tx = 0, ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    const bool insideFrame = tx >= frameX && tx < frameX + frameW && ty >= frameY && ty < frameY + frameH;
    if (!insideFrame) {
      finish();
      return;
    }
    haptic_feedback::touchAction();
    scrollText(tx < frameX + frameW / 3 ? -linesPerPage : linesPerPage);
    return;
  }

  buttonNavigator.onNext([this] { scrollText(linesPerPage); });
  buttonNavigator.onPrevious([this] { scrollText(-linesPerPage); });
}

void TextPopupActivity::render(RenderLock&&) {
  renderer.clearScreen();
  layoutFrame();

  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  if (page) page->render(renderer, fontId, marginLeft, marginTop);
  scope.endScanAndPrewarm();
  if (page) page->render(renderer, fontId, marginLeft, marginTop);

  if (textReady()) {
    drawPopupBody();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
