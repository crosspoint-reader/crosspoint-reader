#include "WriterActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Utf8.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <utility>

#include "Logging.h"
#include "WriterCursor.h"
#include "WriterDraftStore.h"
#include "WriterInput.h"
#include "WriterWrappedLayout.h"
#include "components/UITheme.h"
#include "fontIds.h"

void WriterActivity::onEnter() {
  Activity::onEnter();

  WriterInput::setActive(true);
  inputBuffer.clear();
  showSaveError = false;
  if (!draftStore.ensureDraft()) {
    LOG_ERR("Writer", "Failed to ensure draft file: %s", WriterDraftStore::DraftPath);
  }
  std::string loadedDraft;
  if (draftStore.readDraft(loadedDraft)) {
    draftText = std::move(loadedDraft);
    cursorIndex = WriterCursor::clamp(draftText, draftText.size());
  }
  viewportTopLine = 0;
  clearPreferredCursorX();
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
          utf8RemoveLastChar(draftText);
        }
      } else {
        inputBuffer.push_back(ch);
        draftText.push_back(ch);
      }
    }
    cursorIndex = WriterCursor::clamp(draftText, draftText.size());
    clearPreferredCursorX();
    showSaveError = false;
    markTextChanged();
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

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    moveCursorVertical(-1);
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    moveCursorVertical(1);
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

  const auto& wrappedLines = getWrappedLines(contentWidth);
  const int cursorLine = findWrappedCursorLine(wrappedLines);
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
       lineIndex < static_cast<int>(wrappedLines.size()) && lineIndex < viewportTopLine + maxVisibleLines;
       ++lineIndex) {
    const auto& line = wrappedLines[lineIndex];
    const std::string lineText = getLineText(line);
    renderer.drawText(UI_10_FONT_ID, x, y, lineText.c_str());
    if (lineIndex == cursorLine) {
      const size_t caretOffset = std::clamp(cursorIndex, line.startOffset, line.endOffset);
      const int caretX = x + measureCursorX(line, caretOffset);
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

  const std::string flushedText = inputBuffer;

  // Confirm and Back both route through the same flush path so buffered text
  // is treated consistently whether the user saves in place or exits Writer.
  if (!draftStore.appendToDraft(flushedText)) {
    showSaveError = true;
    LOG_ERR("Writer", "Failed to write to draft file: %s", WriterDraftStore::DraftPath);
    return false;
  }

  inputBuffer.clear();
  showSaveError = false;
  std::string loadedDraft;
  if (draftStore.readDraft(loadedDraft)) {
    draftText = std::move(loadedDraft);
  } else {
    LOG_ERR("Writer", "Draft append succeeded but reload failed; preserving in-memory text");
  }
  cursorIndex = WriterCursor::clamp(draftText, draftText.size());
  markTextChanged();
  clearPreferredCursorX();
  return true;
}

void WriterActivity::markTextChanged() {
  ++textRevision;
  wrappedLayoutCache.valid = false;
}

std::string WriterActivity::getLineText(const WriterWrappedLayout::Line& line) const {
  return draftText.substr(line.startOffset, line.endOffset - line.startOffset);
}

const std::vector<WriterWrappedLayout::Line>& WriterActivity::getWrappedLines(const int contentWidth) {
  if (!wrappedLayoutCache.valid || wrappedLayoutCache.contentWidth != contentWidth ||
      wrappedLayoutCache.textRevision != textRevision) {
    wrappedLayoutCache.contentWidth = contentWidth;
    wrappedLayoutCache.textRevision = textRevision;
    wrappedLayoutCache.lines = WriterWrappedLayout::wrap(
        draftText, contentWidth,
        WriterWrappedLayout::MeasureText{this, [](void* context, const std::string& text) {
                                           auto* activity = static_cast<WriterActivity*>(context);
                                           return activity->renderer.getTextWidth(UI_10_FONT_ID, text.c_str());
                                         }});
    wrappedLayoutCache.valid = true;
  }

  return wrappedLayoutCache.lines;
}

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

void WriterActivity::clearPreferredCursorX() { hasPreferredCursorX = false; }

void WriterActivity::moveCursorLeft() {
  cursorIndex = WriterCursor::moveLeft(draftText, cursorIndex);
  clearPreferredCursorX();
}

void WriterActivity::moveCursorRight() {
  cursorIndex = WriterCursor::moveRight(draftText, cursorIndex);
  clearPreferredCursorX();
}

void WriterActivity::moveCursorVertical(const int lineDelta) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentWidth = renderer.getScreenWidth() - 2 * metrics.contentSidePadding;

  const auto& wrappedLines = getWrappedLines(contentWidth);
  if (wrappedLines.empty()) {
    cursorIndex = 0;
    clearPreferredCursorX();
    return;
  }

  const int currentLine = findWrappedCursorLine(wrappedLines);
  const int targetLine = std::clamp(currentLine + lineDelta, 0, static_cast<int>(wrappedLines.size()) - 1);
  if (targetLine == currentLine) {
    return;
  }

  const size_t clampedCursor = WriterCursor::clamp(draftText, cursorIndex);
  if (!hasPreferredCursorX) {
    preferredCursorX = measureCursorX(wrappedLines[currentLine], clampedCursor);
    hasPreferredCursorX = true;
  }

  cursorIndex = findClosestCursorOffsetOnLine(wrappedLines[targetLine], preferredCursorX);
}

int WriterActivity::findWrappedCursorLine(const std::vector<WriterWrappedLayout::Line>& lines) const {
  if (lines.empty()) {
    return 0;
  }

  const size_t clampedCursor = WriterCursor::clamp(draftText, cursorIndex);
  for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
    const auto& line = lines[i];
    const bool isLastLine = i == static_cast<int>(lines.size()) - 1;
    if (clampedCursor >= line.startOffset && clampedCursor < line.endOffset) {
      return i;
    }

    if (clampedCursor != line.endOffset) {
      continue;
    }

    if (line.startOffset == line.endOffset || isLastLine) {
      return i;
    }

    const auto& nextLine = lines[i + 1];
    if (nextLine.startOffset != clampedCursor) {
      return i;
    }
  }

  return static_cast<int>(lines.size()) - 1;
}

int WriterActivity::measureCursorX(const WriterWrappedLayout::Line& line, const size_t cursorOffset) const {
  const size_t clampedOffset = std::clamp(cursorOffset, line.startOffset, line.endOffset);
  const std::string prefix = draftText.substr(line.startOffset, clampedOffset - line.startOffset);
  return renderer.getTextWidth(UI_10_FONT_ID, prefix.c_str());
}

size_t WriterActivity::findClosestCursorOffsetOnLine(const WriterWrappedLayout::Line& line,
                                                     const int preferredX) const {
  size_t bestOffset = line.startOffset;
  int bestX = measureCursorX(line, bestOffset);
  int bestDistance = std::abs(preferredX - bestX);

  size_t candidate = line.startOffset;

  while (candidate < line.endOffset) {
    const size_t nextCandidate = std::min(WriterCursor::moveRight(draftText, candidate), line.endOffset);
    if (nextCandidate == candidate) {
      break;
    }

    const int nextX = measureCursorX(line, nextCandidate);
    const int distance = std::abs(preferredX - nextX);

    if (distance < bestDistance) {
      bestDistance = distance;
      bestOffset = nextCandidate;
    }
    if (nextX >= preferredX) {
      break;
    }
    candidate = nextCandidate;
  }

  return bestOffset;
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
  const int words = countWords(draftText);
  std::string wordCount = std::to_string(words) + (words == 1 ? " word" : " words");
  int wordCountWidth = renderer.getTextWidth(SMALL_FONT_ID, wordCount.c_str());

  renderer.drawText(SMALL_FONT_ID,
                    renderer.getScreenWidth() - metrics.statusBarHorizontalMargin - footer.marginRight - wordCountWidth,
                    footer.top, wordCount.c_str());

  return;
}
