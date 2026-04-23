#include "WriterActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Utf8.h>

#include <algorithm>
#include <cctype>

#include "Logging.h"
#include "WriterDraftStore.h"
#include "WriterInput.h"
#include "WriterCursor.h"
#include "WriterWrappedLayout.h"
#include "components/UITheme.h"
#include "fontIds.h"

void WriterActivity::onEnter() {
  Activity::onEnter();

  WriterInput::setActive(true);
  inputBuffer.clear();
  showSaveError = false;
  draftStore.ensureDraft();
  draftStore.readDraft(draftText);
  cursorIndex = WriterCursor::clamp(draftText, draftText.size());
  viewportTopLine = 0;
  requestUpdate();
}

void WriterActivity::onExit() {
  WriterInput::setActive(false);
  Activity::onExit();
}

void WriterActivity::loop() {
  std::string inputText;
  if (WriterInput::readAvailable(inputText)) {
    for (const char ch : inputText) {
      if (ch == '\b') {
        if (!inputBuffer.empty()) {
          utf8RemoveLastChar(inputBuffer);
        }
      } else {
        inputBuffer.push_back(ch);
      }
    }
    cursorIndex = getRenderedText().size();
    showSaveError = false;
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    moveCursorLeft();
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    moveCursorRight();
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (!flushInputBuffer()) {
      requestUpdate();
      return;
    }
    finish();
  }
  if (!inputBuffer.empty() && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    flushInputBuffer();
    requestUpdate();
  }
}

void WriterActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto contentWidth = pageWidth - 2 * metrics.contentSidePadding;
  const auto x = metrics.contentSidePadding;
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int textTop = metrics.topPadding;
  const auto footer = getFooterLayout();

  const int availableTextHeight = footer.top - textTop - metrics.verticalSpacing;
  const int maxVisibleLines = std::max(1, availableTextHeight / lineHeight);

  const std::string renderedText = getRenderedText();
  const auto wrappedLines = WriterWrappedLayout::wrap(renderedText, estimateWrapColumns(contentWidth));
  const int cursorLine = findWrappedCursorLine(wrappedLines, renderedText);
  const int maxTopLine = std::max(0, static_cast<int>(wrappedLines.size()) - maxVisibleLines);
  viewportTopLine = std::clamp(viewportTopLine, 0, maxTopLine);
  if (cursorLine < viewportTopLine) {
    viewportTopLine = cursorLine;
  } else if (cursorLine >= viewportTopLine + maxVisibleLines) {
    viewportTopLine = cursorLine - maxVisibleLines + 1;
  }

  // Draw the screen
  int y = metrics.topPadding + metrics.verticalSpacing;
  for (int lineIndex = viewportTopLine;
       lineIndex < static_cast<int>(wrappedLines.size()) && lineIndex < viewportTopLine + maxVisibleLines; ++lineIndex) {
    const auto& line = wrappedLines[lineIndex];
    renderer.drawText(UI_10_FONT_ID, x, y, line.text.c_str());
    if (lineIndex == cursorLine) {
      const size_t caretOffset = std::clamp(cursorIndex, line.startOffset, line.endOffset);
      const std::string prefix = renderedText.substr(line.startOffset, caretOffset - line.startOffset);
      const int caretX = x + renderer.getTextWidth(UI_10_FONT_ID, prefix.c_str());
      const int caretBottom = y + lineHeight - 1;
      renderer.fillRect(caretX, y, 2, lineHeight, true);
      renderer.drawLine(caretX - 2, y, caretX - 1, y, true);
      renderer.drawLine(caretX + 2, y, caretX + 3, y, true);
      renderer.drawLine(caretX - 2, caretBottom, caretX - 1, caretBottom, true);
      renderer.drawLine(caretX + 2, caretBottom, caretX + 3, caretBottom, true);
    }
    y += lineHeight;
  }

  WriterActivity::renderFooter();

  if (showSaveError) {
    GUI.drawPopup(renderer, tr(STR_ERROR_GENERAL_FAILURE));
  }

  renderer.displayBuffer();
}

bool WriterActivity::flushInputBuffer() {
  if (inputBuffer.empty()) {
    showSaveError = false;
    return true;
  }

  // Confirm and Back both route through the same flush path so buffered text
  // is treated consistently whether the user saves in place or exits Writer.
  if (!draftStore.appendToDraft(inputBuffer)) {
    showSaveError = true;
    LOG_ERR("Writer", "Failed to write to draft file: %s", WriterDraftStore::DraftPath);
    return false;
  }

  inputBuffer.clear();
  showSaveError = false;
  draftStore.readDraft(draftText);
  cursorIndex = WriterCursor::clamp(draftText, draftText.size());
  return true;
}

std::string WriterActivity::getRenderedText() const { return draftText + inputBuffer; }

int WriterActivity::countWords(const std::string& text) const {
  int words = 0;
  bool inWord = false;

  for (const unsigned char ch : text) {
    if (std::isspace(ch)) {
      inWord = false;
    } else if (!inWord) {
      words++;
      inWord = true;
    }
  }

  return words;
}

void WriterActivity::moveCursorLeft() { cursorIndex = WriterCursor::moveLeft(getRenderedText(), cursorIndex); }

void WriterActivity::moveCursorRight() { cursorIndex = WriterCursor::moveRight(getRenderedText(), cursorIndex); }

size_t WriterActivity::estimateWrapColumns(const int contentWidth) const {
  const int glyphWidth = std::max(1, renderer.getTextWidth(UI_10_FONT_ID, "M"));
  return std::max<size_t>(1, static_cast<size_t>(contentWidth / glyphWidth));
}

int WriterActivity::findWrappedCursorLine(const std::vector<WriterWrappedLayout::Line>& lines,
                                          const std::string& renderedText) const {
  if (lines.empty()) {
    return 0;
  }

  const size_t clampedCursor = WriterCursor::clamp(renderedText, cursorIndex);
  for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
    const auto& line = lines[i];
    const bool isLastLine = i == static_cast<int>(lines.size()) - 1;
    if (clampedCursor >= line.startOffset && (clampedCursor < line.endOffset || isLastLine)) {
      return i;
    }
    if (clampedCursor == line.endOffset) {
      return i;
    }
  }

  return static_cast<int>(lines.size()) - 1;
}

WriterActivity::FooterLayout WriterActivity::getFooterLayout() const {
  const auto& metrics = UITheme::getInstance().getMetrics();

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  return FooterLayout{renderer.getScreenHeight() - metrics.statusBarVerticalMargin - orientedMarginBottom - 4,
                      orientedMarginLeft, orientedMarginRight};
}

// Standard footer isn't writer-ready, so we'll keep our version local.
void WriterActivity::renderFooter() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto footer = getFooterLayout();

  // Battery display as per user settings
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;

  if (SETTINGS.statusBarBattery) {
    GUI.drawBatteryLeft(renderer,
                        Rect{metrics.statusBarHorizontalMargin + footer.marginLeft + 1, footer.top,
                             metrics.batteryWidth, metrics.batteryHeight},
                        showBatteryPercentage);
  }

  // Filename we're working on
  std::string title = draftStore.getDraftDisplayName();
  int titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
  renderer.drawText(SMALL_FONT_ID, (renderer.getScreenWidth() - titleWidth) / 2, footer.top, title.c_str());

  // Current wordcount
  std::string wordCount = std::to_string(countWords(getRenderedText())) + " words";
  int wordCountWidth = renderer.getTextWidth(SMALL_FONT_ID, wordCount.c_str());

  renderer.drawText(SMALL_FONT_ID,
                    renderer.getScreenWidth() - metrics.statusBarHorizontalMargin - footer.marginRight - wordCountWidth,
                    footer.top, wordCount.c_str());

  return;
}
