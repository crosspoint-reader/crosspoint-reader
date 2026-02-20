#include "ClippingTextViewerActivity.h"

#include <GfxRenderer.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int SKIP_PAGE_MS = 700;
constexpr int LINE_HEIGHT = 32;
constexpr int MARGIN_X = 10;
constexpr int BODY_FONT_ID = BOOKERLY_14_FONT_ID;
}  // namespace

void ClippingTextViewerActivity::wrapText() {
  lines.clear();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int availableWidth = renderer.getScreenWidth() - hintGutterWidth - 2 * MARGIN_X;

  // Process text paragraph by paragraph (split on newlines)
  size_t pos = 0;
  while (pos <= text.size()) {
    // Find next newline or end of string
    size_t nlPos = text.find('\n', pos);
    if (nlPos == std::string::npos) nlPos = text.size();

    std::string paragraph = text.substr(pos, nlPos - pos);
    pos = nlPos + 1;

    // Empty line becomes an empty entry (blank line)
    if (paragraph.empty()) {
      lines.emplace_back("");
      continue;
    }

    // Strip markdown heading prefix; add blank line after for visual separation
    bool isHeading = false;
    if (paragraph.size() >= 3 && paragraph[0] == '#' && paragraph[1] == '#' && paragraph[2] == ' ') {
      paragraph = paragraph.substr(3);
      isHeading = true;
    }

    // Skip markdown separators (---)
    if (paragraph.size() >= 3 && paragraph[0] == '-' && paragraph[1] == '-' && paragraph[2] == '-') {
      continue;
    }

    // Word-wrap the paragraph
    std::string currentLine;
    size_t wordStart = 0;
    while (wordStart < paragraph.size()) {
      // Skip leading spaces
      while (wordStart < paragraph.size() && paragraph[wordStart] == ' ') wordStart++;
      if (wordStart >= paragraph.size()) break;

      // Find end of word
      size_t wordEnd = paragraph.find(' ', wordStart);
      if (wordEnd == std::string::npos) wordEnd = paragraph.size();
      std::string word = paragraph.substr(wordStart, wordEnd - wordStart);
      wordStart = wordEnd;

      if (currentLine.empty()) {
        // First word on the line — always accept it (even if it exceeds width)
        currentLine = word;
      } else {
        std::string candidate = currentLine + " " + word;
        int candidateWidth = renderer.getTextWidth(BODY_FONT_ID, candidate.c_str());
        if (candidateWidth <= availableWidth) {
          currentLine = candidate;
        } else {
          // Push current line, start new one with this word
          lines.push_back(currentLine);
          currentLine = word;
        }
      }
    }

    // Push the last line of the paragraph
    if (!currentLine.empty()) {
      lines.push_back(currentLine);
    }

    // Add blank line after headings for visual separation
    if (isHeading) {
      lines.emplace_back("");
    }
  }
}

void ClippingTextViewerActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  wrapText();

  const auto orientation = renderer.getOrientation();
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  // Content starts at 10 + contentY, footer takes ~30px, so available for text lines:
  const int startY = 10 + hintGutterHeight;
  const int availableHeight = renderer.getScreenHeight() - startY - 30;  // 30px for footer area
  linesPerPage = std::max(1, availableHeight / LINE_HEIGHT);

  requestUpdate();
}

void ClippingTextViewerActivity::onExit() {
  ActivityWithSubactivity::onExit();
}

void ClippingTextViewerActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  const int totalLines = static_cast<int>(lines.size());
  const int maxOffset = std::max(0, totalLines - linesPerPage);

  // Navigation
  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);
  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    onGoBack();
  } else if (prevReleased) {
    const int step = skipPage ? linesPerPage : 1;
    scrollOffset = std::max(0, scrollOffset - step);
    requestUpdate();
  } else if (nextReleased) {
    const int step = skipPage ? linesPerPage : 1;
    scrollOffset = std::min(maxOffset, scrollOffset + step);
    requestUpdate();
  }
}

void ClippingTextViewerActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();

  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;
  const int totalLines = static_cast<int>(lines.size());

  // Draw text lines
  const int endLine = std::min(scrollOffset + linesPerPage, totalLines);
  for (int i = scrollOffset; i < endLine; i++) {
    const int displayY = 10 + contentY + (i - scrollOffset) * LINE_HEIGHT;
    const int textX = contentX + MARGIN_X;
    renderer.drawText(BODY_FONT_ID, textX, displayY, lines[i].c_str(), true);
  }

  // Footer: line status
  char status[32];
  if (totalLines > 0) {
    snprintf(status, sizeof(status), "Line %d-%d of %d", scrollOffset + 1, endLine, totalLines);
  } else {
    snprintf(status, sizeof(status), "Empty");
  }
  renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() - 45, status);

  // Button hints
  const auto labels = mappedInput.mapLabels("\xC2\xAB Back", "", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
