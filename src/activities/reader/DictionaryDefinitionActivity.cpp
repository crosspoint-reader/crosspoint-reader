#include "DictionaryDefinitionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>

#include "CrossPointSettings.h"
#include "DictionaryManager.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

static void toUpperInPlace(char* s) {
  for (int i = 0; s[i] != '\0'; ++i) {
    s[i] = static_cast<char>(toupper(static_cast<unsigned char>(s[i])));
  }
}

DictionaryDefinitionActivity::DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           const char* word, DictResult* results, int resultCount)
    : Activity("DictionaryDefinition", renderer, mappedInput) {
  // Show the normalized (stripped) word in the header, uppercased
  if (!DictionaryManager::normalizeWord(word, searchedWord, sizeof(searchedWord))) {
    snprintf(searchedWord, sizeof(searchedWord), "%s", word);
  }
  toUpperInPlace(searchedWord);

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
  dictManager.scan();
  lookupHistory.load();
  currentResult = 0;
  scrollOffset = 0;
  stackDepth = 0;
  selectionMode = false;
  ignoreNextConfirmRelease = true;  // Absorb stale release from launching activity
  requestUpdate();
}

void DictionaryDefinitionActivity::onExit() { Activity::onExit(); }

void DictionaryDefinitionActivity::loop() {
  // === Long-press Back: exit entirely (fires immediately at threshold) ===
  if (!backLongPressConsumed && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    backLongPressConsumed = true;
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  // === Long-press Confirm: enter selection mode (fires immediately at threshold) ===
  if (!selectionMode && !confirmLongPressConsumed && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    confirmLongPressConsumed = true;
    if (defWordCount > 0) {
      selectionMode = true;
      selectedWordIndex = 0;
      requestUpdate();
    }
    return;
  }

  // === Handle Back release ===
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (backLongPressConsumed) {
      backLongPressConsumed = false;
      return;
    }
    if (ignoreNextBackRelease) {
      ignoreNextBackRelease = false;
      return;
    }

    if (selectionMode) {
      // Exit selection mode only
      selectionMode = false;
      requestUpdate();
      return;
    }

    if (stackDepth > 0) {
      // Pop stack and re-lookup previous word
      popStack();
      return;
    }

    // At bottom of stack — close definition (original behavior)
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  // === Handle Confirm release ===
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (confirmLongPressConsumed) {
      confirmLongPressConsumed = false;
      return;
    }
    if (ignoreNextConfirmRelease) {
      ignoreNextConfirmRelease = false;
      return;
    }

    if (selectionMode) {
      // Look up highlighted word (chained lookup)
      if (selectedWordIndex >= 0 && selectedWordIndex < defWordCount) {
        const auto& w = defWords[selectedWordIndex];
        const auto& r = results[currentResult];
        char wordBuf[256];
        const int len = std::min(static_cast<int>(w.textLen), static_cast<int>(sizeof(wordBuf) - 1));
        memcpy(wordBuf, r.definition + w.textOffset, len);
        wordBuf[len] = '\0';
        performChainedLookup(wordBuf);
      }
      return;
    }

    // Normal mode: Confirm closes definition (original behavior)
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  // === Selection mode: cursor navigation ===
  // Logical buttons are remapped per orientation so physical button positions
  // match the user's visual perspective (see docs/dictionary-button-mapping.md).
  if (selectionMode) {
    if (defWordCount == 0) return;

    const auto orientation = renderer.getOrientation();
    const bool isCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
    const bool isCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
    const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;

    // Map physical buttons to cursor actions based on orientation.
    // Portrait:  Left→Left, Right→Right, Up→Up,    Down→Down
    // Inverted:  Left→Right, Right→Left, Up→Down,  Down→Up
    // CW:        Left→Up,   Right→Down,  Up→Right, Down→Left
    // CCW:       Left→Down, Right→Up,    Up→Left,  Down→Right
    using Btn = MappedInputManager::Button;
    Btn cursorLeftBtn = Btn::Left, cursorRightBtn = Btn::Right;
    Btn cursorUpBtn = Btn::Up, cursorDownBtn = Btn::Down;

    if (isInverted) {
      cursorLeftBtn = Btn::Right;
      cursorRightBtn = Btn::Left;
      cursorUpBtn = Btn::Down;
      cursorDownBtn = Btn::Up;
    } else if (isCw) {
      cursorLeftBtn = Btn::Down;
      cursorRightBtn = Btn::Up;
      cursorUpBtn = Btn::Left;
      cursorDownBtn = Btn::Right;
    } else if (isCcw) {
      cursorLeftBtn = Btn::Up;
      cursorRightBtn = Btn::Down;
      cursorUpBtn = Btn::Right;
      cursorDownBtn = Btn::Left;
    }

    if (mappedInput.wasPressed(cursorRightBtn)) {
      if (selectedWordIndex < defWordCount - 1) {
        selectedWordIndex++;
      } else {
        selectedWordIndex = 0;  // Wrap to first word
      }
      requestUpdate();
    }
    if (mappedInput.wasPressed(cursorLeftBtn)) {
      if (selectedWordIndex > 0) {
        selectedWordIndex--;
      } else {
        selectedWordIndex = defWordCount - 1;  // Wrap to last word
      }
      requestUpdate();
    }
    if (mappedInput.wasPressed(cursorDownBtn)) {
      const int currentY = defWords[selectedWordIndex].y;
      const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
      const int targetY = currentY + lineHeight;
      selectedWordIndex = findWordOnAdjacentLine(selectedWordIndex, targetY);
      requestUpdate();
    }
    if (mappedInput.wasPressed(cursorUpBtn)) {
      const int currentY = defWords[selectedWordIndex].y;
      const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
      const int targetY = currentY - lineHeight;
      selectedWordIndex = findWordOnAdjacentLine(selectedWordIndex, targetY);
      requestUpdate();
    }

    // Scrolling is disabled in selection mode
    return;
  }

  // === Normal mode: orientation-dependent scroll and dict-switch ===
  const auto orientation = renderer.getOrientation();
  const bool isCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;

  using Btn = MappedInputManager::Button;

  Btn scrollBackBtn = Btn::PageBack;
  Btn scrollFwdBtn = Btn::PageForward;
  Btn dictPrevBtn = Btn::Left;
  Btn dictNextBtn = Btn::Right;
  if (isCw) {
    scrollBackBtn = Btn::Left;
    scrollFwdBtn = Btn::Right;
    dictPrevBtn = Btn::Down;
    dictNextBtn = Btn::Up;
  } else if (isCcw) {
    scrollBackBtn = Btn::Right;
    scrollFwdBtn = Btn::Left;
    dictPrevBtn = Btn::Up;
    dictNextBtn = Btn::Down;
  } else if (isInverted) {
    dictPrevBtn = Btn::Right;
    dictNextBtn = Btn::Left;
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
  int headerPos = 0;

  // Show stack depth indicator when chained
  if (stackDepth > 0) {
    headerPos = snprintf(header, sizeof(header), "[%d] ", stackDepth);
  }

  snprintf(header + headerPos, sizeof(header) - headerPos, "%s - %s", searchedWord, r.dictionaryName);

  const int headerY = metrics.topPadding + hintGutterHeight;
  GUI.drawHeader(renderer, Rect{contentX, headerY, availableWidth, metrics.headerHeight}, header);

  const int contentTop = headerY + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  drawDefinition(contentTop, contentX + margin, contentWidth, contentHeight, rightGutter);

  if (selectionMode) {
    // Selection mode: labels match orientation-remapped cursor directions.
    // See docs/dictionary-button-mapping.md for the full mapping table.
    const bool isLandscape = isLandscapeCw || isLandscapeCcw;

    const char* leftLabel;    // label at physical Left button position
    const char* rightLabel;   // label at physical Right button position
    const char* topLabel;     // label at physical BTN_UP position
    const char* bottomLabel;  // label at physical BTN_DOWN position

    if (isLandscapeCw) {
      leftLabel = tr(STR_DIR_UP);
      rightLabel = tr(STR_DIR_DOWN);
      topLabel = tr(STR_DIR_RIGHT);
      bottomLabel = tr(STR_DIR_LEFT);
    } else if (isLandscapeCcw) {
      leftLabel = tr(STR_DIR_DOWN);
      rightLabel = tr(STR_DIR_UP);
      topLabel = tr(STR_DIR_LEFT);
      bottomLabel = tr(STR_DIR_RIGHT);
    } else if (isPortraitInverted) {
      leftLabel = tr(STR_DIR_RIGHT);
      rightLabel = tr(STR_DIR_LEFT);
      topLabel = tr(STR_DIR_DOWN);
      bottomLabel = tr(STR_DIR_UP);
    } else {
      // Portrait (default)
      leftLabel = tr(STR_DIR_LEFT);
      rightLabel = tr(STR_DIR_RIGHT);
      topLabel = tr(STR_DIR_UP);
      bottomLabel = tr(STR_DIR_DOWN);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_LOOKUP), leftLabel, rightLabel);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    if (isLandscape || isPortraitInverted) {
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      GUI.drawSideButtonHints(renderer, topLabel, bottomLabel);
      renderer.setOrientation(orientation);
    } else {
      GUI.drawSideButtonHints(renderer, topLabel, bottomLabel);
    }
  } else {
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
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT_PAREN), leftLabel, rightLabel);
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
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT_PAREN), leftLabel, rightLabel);
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
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT_PAREN), leftLabel, rightLabel);
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

      if (canScroll) {
        const char* topLabel = swapped ? tr(STR_DIR_DOWN) : tr(STR_DIR_UP);
        const char* bottomLabel = swapped ? tr(STR_DIR_UP) : tr(STR_DIR_DOWN);
        GUI.drawSideButtonHints(renderer, topLabel, bottomLabel);
      }
    }
  }  // end selection mode else

  // Draw selection highlight if in selection mode
  renderSelectionHighlight();

  renderer.displayBuffer();
}

void DictionaryDefinitionActivity::drawDefinition(int contentTop, int contentLeft, int contentWidth, int contentHeight,
                                                  int rightGutter) {
  if (currentResult < 0 || currentResult >= resultCount || !results) return;

  const auto& r = results[currentResult];
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  if (lineHeight <= 0) return;

  const int margin = contentLeft;
  // Reserve one line at bottom for scroll indicator so it doesn't overlap text
  const int usableHeight = contentHeight - lineHeight;
  this->maxVisibleLines = usableHeight / lineHeight;

  // Reset word extraction for selection mode
  defWordCount = 0;

  // Word-wrap the definition text into lines
  const char* text = r.definition;
  const int textLen = static_cast<int>(strlen(text));

  // Word-by-word wrap: measure incrementally by adding words, not characters.
  int lineIdx = 0;
  int pos = 0;
  while (pos < textLen) {
    int lineEnd = pos;
    int lastSpace = -1;
    char lineBuf[256];

    while (lineEnd < textLen && text[lineEnd] != '\n') {
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
      if (lineEnd < textLen && text[lineEnd] == ' ') lineEnd++;
    }

    if (lineEnd < textLen && text[lineEnd] == '\n') {
      // Include up to but not including the newline
    } else if (lineEnd == pos && pos < textLen) {
      lineEnd = pos + 1;
    }

    // Render if within visible range
    if (lineIdx >= scrollOffset && lineIdx < scrollOffset + this->maxVisibleLines) {
      const int drawY = contentTop + (lineIdx - scrollOffset) * lineHeight;
      const int segLen = std::min(lineEnd - pos, static_cast<int>(sizeof(lineBuf) - 1));
      memcpy(lineBuf, text + pos, segLen);
      lineBuf[segLen] = '\0';
      renderer.drawText(UI_12_FONT_ID, margin, drawY, lineBuf, true);

      // Extract word positions for selection mode.
      // getTextWidth returns visual bounding box width, so a prefix ending with
      // a space (e.g. "hello ") won't include the space's advance.  To get the
      // correct x of a word we measure "prefix-through-first-char-of-word" and
      // subtract the first character's visual contribution.
      if (defWordCount < MAX_DEF_WORDS) {
        int wordStart = pos;
        while (wordStart < pos + segLen) {
          while (wordStart < pos + segLen && text[wordStart] == ' ') wordStart++;
          if (wordStart >= pos + segLen) break;

          int wEnd = wordStart;
          while (wEnd < pos + segLen && text[wEnd] != ' ') wEnd++;

          const int wordLen = wEnd - wordStart;
          if (wordLen > 0 && defWordCount < MAX_DEF_WORDS) {
            int wordX;
            if (wordStart == pos) {
              // First word on the line — starts at margin
              wordX = margin;
            } else {
              // Measure prefix through the first char of this word.
              // This captures the space advance because it's followed by a
              // non-space character whose glyph extends maxX in getTextBounds.
              char prefixBuf[256];
              const int prefixLen = std::min(wordStart - pos + 1, static_cast<int>(sizeof(prefixBuf) - 1));
              memcpy(prefixBuf, text + pos, prefixLen);
              prefixBuf[prefixLen] = '\0';
              const int prefixWidth = renderer.getTextWidth(UI_12_FONT_ID, prefixBuf);

              // Measure just the first character to subtract its contribution
              char firstChar[8];
              const int fcLen = 1;  // ASCII assumption OK for definition text
              memcpy(firstChar, text + wordStart, fcLen);
              firstChar[fcLen] = '\0';
              const int firstCharWidth = renderer.getTextWidth(UI_12_FONT_ID, firstChar);

              wordX = margin + prefixWidth - firstCharWidth;
            }

            char wordBuf[256];
            const int wbLen = std::min(wordLen, static_cast<int>(sizeof(wordBuf) - 1));
            memcpy(wordBuf, text + wordStart, wbLen);
            wordBuf[wbLen] = '\0';
            const int wordWidth = renderer.getTextWidth(UI_12_FONT_ID, wordBuf);

            defWords[defWordCount].x = static_cast<int16_t>(wordX);
            defWords[defWordCount].y = static_cast<int16_t>(drawY);
            defWords[defWordCount].width = static_cast<int16_t>(wordWidth);
            defWords[defWordCount].textOffset = static_cast<int16_t>(wordStart);
            defWords[defWordCount].textLen = static_cast<int16_t>(wordLen);
            defWordCount++;
          }
          wordStart = wEnd;
        }
      }
    }

    lineIdx++;
    pos = lineEnd;
    if (pos < textLen && (text[pos] == ' ' || text[pos] == '\n')) pos++;
  }

  this->totalLines = lineIdx;

  // Draw page indicator if content overflows
  if (totalLines > maxVisibleLines) {
    const int totalPages = (totalLines + maxVisibleLines - 1) / maxVisibleLines;
    const int currentPage = (scrollOffset / std::max(1, maxVisibleLines)) + 1;
    char indicator[32];
    snprintf(indicator, sizeof(indicator), "%d/%d", currentPage, totalPages);
    const int indicatorWidth = renderer.getTextWidth(UI_12_FONT_ID, indicator);
    renderer.drawText(UI_12_FONT_ID, contentLeft + contentWidth - indicatorWidth,
                      contentTop + contentHeight - lineHeight, indicator, true);
  }
}

int DictionaryDefinitionActivity::findWordOnAdjacentLine(int currentIdx, int targetY) const {
  if (currentIdx < 0 || currentIdx >= defWordCount) return currentIdx;

  const int currentY = defWords[currentIdx].y;
  const int currentX = defWords[currentIdx].x;
  const bool goingDown = targetY > currentY;

  // Find the nearest line with words in the given direction.
  // This handles blank lines (newlines with no words) by skipping past them.
  int nearestLineY = -1;
  int nearestLineDist = INT_MAX;

  for (int i = 0; i < defWordCount; ++i) {
    const int wy = defWords[i].y;
    if (goingDown && wy > currentY) {
      const int dist = wy - currentY;
      if (dist < nearestLineDist) {
        nearestLineDist = dist;
        nearestLineY = wy;
      }
    } else if (!goingDown && wy < currentY) {
      const int dist = currentY - wy;
      if (dist < nearestLineDist) {
        nearestLineDist = dist;
        nearestLineY = wy;
      }
    }
  }

  if (nearestLineY < 0) return currentIdx;  // No line found in that direction

  // Find the word on that line closest to the current x position
  int bestIdx = -1;
  int bestDist = INT_MAX;
  for (int i = 0; i < defWordCount; ++i) {
    if (defWords[i].y != nearestLineY) continue;
    const int dist = abs(defWords[i].x - currentX);
    if (dist < bestDist) {
      bestDist = dist;
      bestIdx = i;
    }
  }
  return bestIdx >= 0 ? bestIdx : currentIdx;
}

void DictionaryDefinitionActivity::renderSelectionHighlight() {
  if (!selectionMode || defWordCount == 0) return;
  if (selectedWordIndex < 0 || selectedWordIndex >= defWordCount) return;

  const auto& w = defWords[selectedWordIndex];
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int padding = 2;

  // Draw inverted rectangle behind the word (true = black fill)
  renderer.fillRect(w.x - padding, w.y, w.width + padding * 2, lineHeight, true);

  // Draw word text in white (inverted)
  const auto& r = results[currentResult];
  char wordBuf[256];
  const int len = std::min(static_cast<int>(w.textLen), static_cast<int>(sizeof(wordBuf) - 1));
  memcpy(wordBuf, r.definition + w.textOffset, len);
  wordBuf[len] = '\0';
  renderer.drawText(UI_12_FONT_ID, w.x, w.y, wordBuf, false);  // false = white text
}

void DictionaryDefinitionActivity::performChainedLookup(const char* word) {
  // Don't push if stack is full
  if (stackDepth >= MAX_CHAIN_DEPTH) return;

  // Normalize the word for lookup
  char normalized[DICT_WORD_MAX];
  if (!DictionaryManager::normalizeWord(word, normalized, sizeof(normalized))) {
    snprintf(normalized, sizeof(normalized), "%.*s", DICT_WORD_MAX - 1, word);
  }

  // Record to lookup history
  lookupHistory.addWord(normalized);
  lookupHistory.save();

  // Allocate results for the new lookup
  auto* newResults = static_cast<DictResult*>(malloc(sizeof(DictResult) * DictionaryManager::MAX_RESULTS));
  if (!newResults) {
    LOG_ERR("DICT", "malloc failed for chained lookup results");
    return;
  }

  const int newResultCount = dictManager.lookup(normalized, newResults, DictionaryManager::MAX_RESULTS);

  if (newResultCount == 0) {
    free(newResults);
    // Show "no definition found" popup without modifying the stack
    GUI.drawPopup(renderer, tr(STR_NO_DEFINITION_FOUND));
    renderer.displayBuffer();
    delay(1500);
    requestUpdate();
    return;
  }

  // Push current word onto stack
  strncpy(wordStack[stackDepth], searchedWord, DICT_WORD_MAX - 1);
  wordStack[stackDepth][DICT_WORD_MAX - 1] = '\0';
  stackDepth++;

  // Replace current state with new lookup
  free(results);
  results = newResults;
  resultCount = newResultCount;
  currentResult = 0;
  scrollOffset = 0;
  selectionMode = false;

  // Update searchedWord (uppercased for header display)
  snprintf(searchedWord, sizeof(searchedWord), "%s", normalized);
  toUpperInPlace(searchedWord);

  requestUpdate();
}

void DictionaryDefinitionActivity::popStack() {
  if (stackDepth <= 0) return;

  const char* prevWord = wordStack[stackDepth - 1];  // Peek, don't pop yet

  // Allocate before decrementing — if malloc fails, state stays consistent
  auto* newResults = static_cast<DictResult*>(malloc(sizeof(DictResult) * DictionaryManager::MAX_RESULTS));
  if (!newResults) {
    LOG_ERR("DICT", "malloc failed for pop lookup results");
    return;
  }

  stackDepth--;  // Only decrement after successful allocation

  const int newResultCount = dictManager.lookup(prevWord, newResults, DictionaryManager::MAX_RESULTS);

  // Replace current state
  free(results);
  results = newResults;
  resultCount = newResultCount;
  currentResult = 0;
  scrollOffset = 0;

  // Restore searchedWord
  snprintf(searchedWord, sizeof(searchedWord), "%s", prevWord);

  requestUpdate();
}
