#include "DictionaryDefinitionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "CrossPointSettings.h"
#include "DictionaryManager.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

DictionaryDefinitionActivity::DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           const char* word, DictResult* results, int resultCount)
    : Activity("DictionaryDefinition", renderer, mappedInput) {
  // Show the normalized (stripped) word in the header, uppercased
  if (!DictionaryManager::normalizeWord(word, searchedWord, sizeof(searchedWord))) {
    snprintf(searchedWord, sizeof(searchedWord), "%s", word);
  }
  for (int i = 0; searchedWord[i] != '\0'; ++i) {
    searchedWord[i] = static_cast<char>(toupper(static_cast<unsigned char>(searchedWord[i])));
  }

  // Takes ownership of the heap-allocated results array
  if (resultCount > 0 && results) {
    this->results = results;
    this->resultCount = resultCount;
  } else {
    free(results);  // Free if empty/null (caller still expects ownership transfer)
    this->results = nullptr;
    this->resultCount = 0;
  }
}

DictionaryDefinitionActivity::~DictionaryDefinitionActivity() { free(results); }

void DictionaryDefinitionActivity::onEnter() {
  Activity::onEnter();
  currentResult = 0;
  scrollOffset = 0;
  requestUpdate();
}

void DictionaryDefinitionActivity::onExit() { Activity::onExit(); }

void DictionaryDefinitionActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  const auto orientation = renderer.getOrientation();
  const bool isCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;

  // Button roles depend on orientation because the physical buttons rotate with the device.
  // Use Button::Up/Down directly (not PageBack/PageForward) for landscape dict switching
  // to match EpubReaderActivity's nav mapping and avoid sideButtonLayout swap interference.
  using Btn = MappedInputManager::Button;

  Btn scrollBackBtn = Btn::PageBack;
  Btn scrollFwdBtn = Btn::PageForward;
  Btn dictPrevBtn = Btn::Left;
  Btn dictNextBtn = Btn::Right;
  if (isCw) {
    scrollBackBtn = Btn::Left;
    scrollFwdBtn = Btn::Right;
    // CW: BTN_DOWN = user's LEFT (prev), BTN_UP = user's RIGHT (next)
    dictPrevBtn = Btn::Down;
    dictNextBtn = Btn::Up;
  } else if (isCcw) {
    scrollBackBtn = Btn::Right;
    scrollFwdBtn = Btn::Left;
    // CCW: BTN_UP = user's LEFT (prev), BTN_DOWN = user's RIGHT (next)
    dictPrevBtn = Btn::Up;
    dictNextBtn = Btn::Down;
  } else if (isInverted) {
    // Inverted: physical Left = user's RIGHT, physical Right = user's LEFT
    dictPrevBtn = Btn::Right;
    dictNextBtn = Btn::Left;
    // BTN_DOWN = user's upper-left → scroll back, BTN_UP = user's lower-left → scroll forward
    scrollBackBtn = Btn::Down;
    scrollFwdBtn = Btn::Up;
  }

  if (mappedInput.wasPressed(scrollBackBtn)) {
    if (maxVisibleLines > 0) {
      const int currentPage = scrollOffset / maxVisibleLines;
      if (currentPage > 0) {
        scrollOffset = (currentPage - 1) * maxVisibleLines;
        requestUpdate();
      }
    }
  }
  if (mappedInput.wasPressed(scrollFwdBtn)) {
    if (maxVisibleLines > 0) {
      const int totalPages = (totalLines + maxVisibleLines - 1) / maxVisibleLines;
      const int currentPage = scrollOffset / maxVisibleLines;
      if (currentPage < totalPages - 1) {
        scrollOffset = (currentPage + 1) * maxVisibleLines;
        requestUpdate();
      }
    }
  }

  if (mappedInput.wasPressed(dictPrevBtn)) {
    if (currentResult > 0) {
      currentResult--;
      scrollOffset = 0;
      requestUpdate();
    }
  }
  if (mappedInput.wasPressed(dictNextBtn)) {
    if (currentResult < resultCount - 1) {
      currentResult++;
      scrollOffset = 0;
      requestUpdate();
    }
  }
}

void DictionaryDefinitionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Reserve gutters so button hints don't overlap content.
  // - Landscape: front button hints on one side edge (30px)
  // - Portrait inverted: front button hints at top (50px), side hints on LEFT edge
  // - Portrait: side button hints on RIGHT edge (sideButtonHintsWidth)
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortrait = orientation == GfxRenderer::Orientation::Portrait;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int frontHintGutter = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int sideHintGutter = (isPortrait || isPortraitInverted) ? metrics.sideButtonHintsWidth : 0;
  // In inverted portrait, side buttons are on the LEFT (physical right edge = user's left).
  // In CW landscape, front button hints are on the left edge.
  const int leftGutter = (isPortraitInverted ? sideHintGutter : 0) + (isLandscapeCw ? frontHintGutter : 0);
  const int rightGutter = (isPortrait ? sideHintGutter : 0) + (isLandscapeCcw ? frontHintGutter : 0);
  const int contentX = leftGutter;
  const int availableWidth = pageWidth - leftGutter - rightGutter;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;

  const int margin = metrics.contentSidePadding;
  const int contentWidth = availableWidth - margin * 2;

  if (resultCount == 0 || !results) {
    // No definition found
    char header[96];
    snprintf(header, sizeof(header), "%s", searchedWord);
    GUI.drawHeader(renderer,
                   Rect{contentX, metrics.topPadding + hintGutterHeight, availableWidth, metrics.headerHeight}, header);

    const int centerY = pageHeight / 2 - 10;
    renderer.drawCenteredText(UI_12_FONT_ID, centerY, tr(STR_NO_DEFINITION_FOUND), true);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const auto& r = results[currentResult];
  char header[128];
  if (resultCount > 1) {
    snprintf(header, sizeof(header), "(%d/%d) %s - %s", currentResult + 1, resultCount, searchedWord, r.dictionaryName);
  } else {
    snprintf(header, sizeof(header), "%s - %s", searchedWord, r.dictionaryName);
  }

  GUI.drawHeader(renderer, Rect{contentX, metrics.topPadding + hintGutterHeight, availableWidth, metrics.headerHeight},
                 header);

  const int contentTop = metrics.topPadding + hintGutterHeight + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  drawDefinition(contentTop, contentX + margin, contentWidth, contentHeight, rightGutter);

  // Button roles depend on orientation (see loop() for matching input logic).
  // drawSideButtonHints only works in portrait coordinate space, so we force
  // portrait orientation before calling it in landscape/inverted modes.
  const bool isLandscape = isLandscapeCw || isLandscapeCcw;
  const bool multiResult = resultCount > 1;
  const bool canScroll = totalLines > maxVisibleLines;
  const bool swapped = SETTINGS.sideButtonLayout == CrossPointSettings::NEXT_PREV;

  if (isLandscape) {
    // Front Left/Right: scroll pages (CW: Left=up, Right=down; CCW: reversed)
    const char* upLabel = canScroll ? tr(STR_DIR_UP) : "";
    const char* downLabel = canScroll ? tr(STR_DIR_DOWN) : "";
    const char* leftLabel = isLandscapeCw ? upLabel : downLabel;
    const char* rightLabel = isLandscapeCw ? downLabel : upLabel;
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", leftLabel, rightLabel);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    // Side buttons: switch dictionaries (using Up/Down directly, no swap).
    // CW: topBtn (BTN_UP) drawn on RIGHT = next, bottomBtn (BTN_DOWN) drawn on LEFT = prev.
    // CCW: topBtn (BTN_UP) drawn on LEFT = prev, bottomBtn (BTN_DOWN) drawn on RIGHT = next.
    if (multiResult) {
      const char* topLabel;
      const char* bottomLabel;
      if (isLandscapeCw) {
        topLabel = (currentResult < resultCount - 1) ? ">" : "";
        bottomLabel = (currentResult > 0) ? "<" : "";
      } else {
        // CCW: physical positions are swapped from CW — swap conditions, keep symbols.
        topLabel = (currentResult > 0) ? ">" : "";
        bottomLabel = (currentResult < resultCount - 1) ? "<" : "";
      }
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      GUI.drawSideButtonHints(renderer, topLabel, bottomLabel);
      renderer.setOrientation(orientation);
    }
  } else if (isPortraitInverted) {
    // drawButtonHints forces Portrait, so labels land at physical button positions.
    // In inverted, physical Left = Dict Next, physical Right = Dict Prev.
    // Swap conditions (not symbols) to match the inverted function mapping.
    const char* leftLabel = (multiResult && currentResult < resultCount - 1) ? "<" : "";
    const char* rightLabel = (multiResult && currentResult > 0) ? ">" : "";
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", leftLabel, rightLabel);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    // Side buttons: scroll pages (using Up/Down directly, no swap).
    // BTN_UP (topBtn) = user's lower-left → scroll forward (Down).
    // BTN_DOWN (bottomBtn) = user's upper-left → scroll back (Up).
    // Force Portrait so drawSideButtonHints draws at the correct physical edge (user's left).
    if (canScroll) {
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      GUI.drawSideButtonHints(renderer, tr(STR_DIR_DOWN), tr(STR_DIR_UP));
      renderer.setOrientation(orientation);
    }
  } else {
    const char* leftLabel = (multiResult && currentResult > 0) ? "<" : "";
    const char* rightLabel = (multiResult && currentResult < resultCount - 1) ? ">" : "";
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", leftLabel, rightLabel);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    if (canScroll) {
      const char* topLabel = swapped ? tr(STR_DIR_DOWN) : tr(STR_DIR_UP);
      const char* bottomLabel = swapped ? tr(STR_DIR_UP) : tr(STR_DIR_DOWN);
      GUI.drawSideButtonHints(renderer, topLabel, bottomLabel);
    }
  }

  renderer.displayBuffer();
}

void DictionaryDefinitionActivity::drawDefinition(int contentTop, int contentLeft, int contentWidth,
                                                  int contentHeight, int rightGutter) {
  if (currentResult < 0 || currentResult >= resultCount || !results) return;

  const auto& r = results[currentResult];
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  if (lineHeight <= 0) return;

  const int margin = contentLeft;
  // Reserve one line at bottom for scroll indicator so it doesn't overlap text
  const int usableHeight = contentHeight - lineHeight;
  this->maxVisibleLines = usableHeight / lineHeight;

  // Word-wrap the definition text into lines
  const char* text = r.definition;
  const int textLen = static_cast<int>(strlen(text));

  // Word-by-word wrap: measure incrementally by adding words, not characters.
  // This reduces getTextWidth calls from O(chars_per_line) to O(words_per_line).
  int lineIdx = 0;
  int pos = 0;
  while (pos < textLen) {
    int lineEnd = pos;
    int lastSpace = -1;
    char lineBuf[256];

    while (lineEnd < textLen && text[lineEnd] != '\n') {
      // Advance to end of next word (or end of text/line)
      int wordEnd = lineEnd;
      while (wordEnd < textLen && text[wordEnd] != ' ' && text[wordEnd] != '\n') wordEnd++;

      const int segLen = wordEnd - pos;
      if (segLen >= static_cast<int>(sizeof(lineBuf) - 1)) break;
      memcpy(lineBuf, text + pos, segLen);
      lineBuf[segLen] = '\0';
      const int w = renderer.getTextWidth(UI_12_FONT_ID, lineBuf);
      if (w > contentWidth && lastSpace >= 0) {
        lineEnd = lastSpace;
        break;
      }
      if (wordEnd < textLen && text[wordEnd] == ' ') lastSpace = wordEnd;
      lineEnd = wordEnd;
      // Skip the space to start measuring the next word
      if (lineEnd < textLen && text[lineEnd] == ' ') lineEnd++;
    }

    // Handle newline
    if (lineEnd < textLen && text[lineEnd] == '\n') {
      // Include up to but not including the newline
    } else if (lineEnd == pos && pos < textLen) {
      // Single word too wide, advance by one character to avoid infinite loop
      lineEnd = pos + 1;
    }

    // Render if within visible range
    if (lineIdx >= scrollOffset && lineIdx < scrollOffset + this->maxVisibleLines) {
      const int drawY = contentTop + (lineIdx - scrollOffset) * lineHeight;
      const int segLen = std::min(lineEnd - pos, static_cast<int>(sizeof(lineBuf) - 1));
      memcpy(lineBuf, text + pos, segLen);
      lineBuf[segLen] = '\0';
      renderer.drawText(UI_12_FONT_ID, margin, drawY, lineBuf, true);
    }

    lineIdx++;
    pos = lineEnd;
    // Skip space or newline at the break point
    if (pos < textLen && (text[pos] == ' ' || text[pos] == '\n')) pos++;
  }

  // Store total lines for scroll clamping in loop()
  this->totalLines = lineIdx;

  // Draw page indicator if content overflows
  if (totalLines > maxVisibleLines) {
    const int totalPages = (totalLines + maxVisibleLines - 1) / maxVisibleLines;
    const int currentPage = (scrollOffset / std::max(1, maxVisibleLines)) + 1;
    char indicator[32];
    snprintf(indicator, sizeof(indicator), "%d/%d", currentPage, totalPages);
    const int indicatorWidth = renderer.getTextWidth(UI_12_FONT_ID, indicator);
    renderer.drawText(UI_12_FONT_ID, renderer.getScreenWidth() - indicatorWidth - 10 - rightGutter,
                      contentTop + contentHeight - lineHeight, indicator, true);
  }
}
