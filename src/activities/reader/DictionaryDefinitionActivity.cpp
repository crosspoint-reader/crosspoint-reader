#include "DictionaryDefinitionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

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

  // Up/Down: scroll definition text (clamp in loop, not render)
  buttonNavigator.onNextRelease([this] {
    const int maxScroll = std::max(0, totalLines - maxVisibleLines);
    if (scrollOffset < maxScroll) {
      scrollOffset++;
      requestUpdate();
    }
  });

  buttonNavigator.onPreviousRelease([this] {
    if (scrollOffset > 0) {
      scrollOffset--;
      requestUpdate();
    }
  });

  // Left/Right: cycle between dictionary results
  if (resultCount > 1) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (currentResult > 0) {
        currentResult--;
        scrollOffset = 0;
        requestUpdate();
      }
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (currentResult < resultCount - 1) {
        currentResult++;
        scrollOffset = 0;
        requestUpdate();
      }
    }
  }
}

void DictionaryDefinitionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // In landscape/inverted modes, reserve a gutter so button hints don't overlap content.
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int availableWidth = pageWidth - hintGutterWidth;
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

  // Build header: "< Word - Dictionary >" with arrows
  const auto& r = results[currentResult];
  char header[128];
  if (resultCount == 1) {
    snprintf(header, sizeof(header), "%s - %s", searchedWord, r.dictionaryName);
  } else if (currentResult == 0) {
    snprintf(header, sizeof(header), "%s - %s >", searchedWord, r.dictionaryName);
  } else if (currentResult == resultCount - 1) {
    snprintf(header, sizeof(header), "< %s - %s", searchedWord, r.dictionaryName);
  } else {
    snprintf(header, sizeof(header), "< %s - %s >", searchedWord, r.dictionaryName);
  }

  GUI.drawHeader(renderer, Rect{contentX, metrics.topPadding + hintGutterHeight, availableWidth, metrics.headerHeight},
                 header);

  const int contentTop = metrics.topPadding + hintGutterHeight + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  drawDefinition(contentTop, contentX + margin, contentWidth, contentHeight);

  // Button hints: Up/Down scroll, Back dismisses
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void DictionaryDefinitionActivity::drawDefinition(int contentTop, int contentLeft, int contentWidth,
                                                  int contentHeight) {
  if (currentResult < 0 || currentResult >= resultCount || !results) return;

  const auto& r = results[currentResult];
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  if (lineHeight <= 0) return;

  const int margin = contentLeft;
  this->maxVisibleLines = contentHeight / lineHeight;

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

  // Draw scroll indicator if content overflows
  if (totalLines > maxVisibleLines) {
    char indicator[32];
    snprintf(indicator, sizeof(indicator), "%d/%d", scrollOffset + 1, std::max(1, totalLines - maxVisibleLines + 1));
    const int indicatorWidth = renderer.getTextWidth(UI_12_FONT_ID, indicator);
    renderer.drawText(UI_12_FONT_ID, renderer.getScreenWidth() - indicatorWidth - 10,
                      contentTop + contentHeight - lineHeight, indicator, true);
  }
}
