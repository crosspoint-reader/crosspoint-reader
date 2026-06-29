#include "WordSelectNavigator.h"

#include <GfxRenderer.h>
#include <Utf8.h>

#include <cstdlib>

#include "MappedInputManager.h"
#include "TextPool.h"

void WordSelectNavigator::load(std::vector<WordInfo> w, std::vector<Row> r, std::string pool,
                               bool consumeInitialConfirm, int initialPreferredX) {
  words = std::move(w);
  rows = std::move(r);
  textPool = std::move(pool);
  currentRow = static_cast<int>(rows.size()) / 2;
  currentWordInRow = 0;
  inMultiSelectMode = false;
  anchorFlatIndex = -1;
  pendingSnapIdx = -1;
  if (!rows.empty() && !rows[currentRow].wordIndices.empty()) {
    currentWordInRow = findClosestWordFromX(currentRow, initialPreferredX);
    preferredRowNavX = initialPreferredX;
  } else {
    preferredRowNavX = initialPreferredX;
  }
  confirmReleaseConsumed = consumeInitialConfirm;
}

void WordSelectNavigator::organizeIntoRows(std::vector<WordInfo>& words, std::vector<Row>& rows) {
  if (words.empty()) return;
  int16_t currentY = words[0].screenY;
  rows.push_back({currentY, {}});
  for (size_t i = 0; i < words.size(); i++) {
    if (std::abs(words[i].screenY - currentY) > 2) {
      currentY = words[i].screenY;
      rows.push_back({currentY, {}});
    }
    words[i].row = static_cast<int16_t>(rows.size() - 1);
    rows.back().wordIndices.push_back(static_cast<int>(i));
  }
}

void WordSelectNavigator::mergeHyphenatedPairs(std::vector<WordInfo>& words, const std::vector<Row>& rows,
                                               std::string& textPool) {
  for (size_t r = 0; r + 1 < rows.size(); r++) {
    if (rows[r].wordIndices.empty() || rows[r + 1].wordIndices.empty()) continue;

    int lastWordIdx = rows[r].wordIndices.back();
    const char* lastWord = textPool.data() + words[lastWordIdx].textOffset;
    uint16_t lastLen = words[lastWordIdx].textLen;
    if (lastLen == 0) continue;
    if (!utf8EndsWithHyphen(lastWord, lastLen)) continue;
    // A word that also starts with '-' (e.g. -re-) is a standalone affix token,
    // not the first half of a line-break compound.
    if (lastWord[0] == '-') continue;

    int nextWordIdx = rows[r + 1].wordIndices.front();
    words[lastWordIdx].continuationIndex = nextWordIdx;
    words[nextWordIdx].continuationOf = lastWordIdx;

    std::string firstPart(lastWord, lastLen);
    utf8RemoveTrailingHyphen(firstPart);
    const char* nextWord = textPool.data() + words[nextWordIdx].textOffset;
    const char* strippedNext = (nextWord[0] == '-') ? nextWord + 1 : nextWord;
    std::string merged = firstPart + strippedNext;
    uint16_t mergedOff = poolAppend(textPool, merged.c_str(), merged.size());
    words[lastWordIdx].lookupOffset = mergedOff;
    words[lastWordIdx].lookupLen = static_cast<uint16_t>(merged.size());
    words[nextWordIdx].lookupOffset = mergedOff;
    words[nextWordIdx].lookupLen = static_cast<uint16_t>(merged.size());
  }
}

uint16_t WordSelectNavigator::poolAppend(std::string& pool, const char* s, size_t len) {
  return TextPool::append(pool, s, len);
}

void WordSelectNavigator::appendWord(std::vector<WordInfo>& words, std::string& pool, const char* display,
                                     size_t displayLen, const char* lookup, size_t lookupLen, int16_t screenX,
                                     int16_t screenY, int16_t width, EpdFontFamily::Style style, int fontId,
                                     bool isDictFont) {
  const uint16_t textOff = poolAppend(pool, display, displayLen);
  // lookup == nullptr: display text is also the lookup key; reuse its offset
  // instead of appending a duplicate copy to the pool.
  uint16_t lookupOff = textOff;
  uint16_t lookupL = static_cast<uint16_t>(displayLen);
  if (lookup) {
    lookupOff = poolAppend(pool, lookup, lookupLen);
    lookupL = static_cast<uint16_t>(lookupLen);
  }

  WordInfo wi;
  wi.textOffset = textOff;
  wi.textLen = static_cast<uint16_t>(displayLen);
  wi.lookupOffset = lookupOff;
  wi.lookupLen = lookupL;
  wi.screenX = screenX;
  wi.screenY = screenY;
  wi.width = width;
  wi.style = style;
  wi.fontId = fontId;
  wi.isDictFont = isDictFont;
  words.push_back(wi);
}

void WordSelectNavigator::reset() {
  words.clear();
  rows.clear();
  textPool.clear();
  currentRow = 0;
  currentWordInRow = 0;
  inMultiSelectMode = false;
  confirmReleaseConsumed = false;
  anchorFlatIndex = -1;
  pendingSnapIdx = -1;
  preferredRowNavX = -1;
}

const WordSelectNavigator::WordInfo* WordSelectNavigator::getSelected() const {
  if (rows.empty() || currentRow >= static_cast<int>(rows.size())) return nullptr;
  if (rows[currentRow].wordIndices.empty()) return nullptr;
  return &words[rows[currentRow].wordIndices[currentWordInRow]];
}

const WordSelectNavigator::WordInfo* WordSelectNavigator::getPairedHalf() const {
  const WordInfo* sel = getSelected();
  if (!sel) return nullptr;
  const int wordIdx = rows[currentRow].wordIndices[currentWordInRow];
  int otherIdx = (sel->continuationOf >= 0) ? sel->continuationOf : -1;
  if (otherIdx < 0 && sel->continuationIndex >= 0 && sel->continuationIndex != wordIdx) {
    otherIdx = sel->continuationIndex;
  }
  if (otherIdx >= 0 && otherIdx < static_cast<int>(words.size())) {
    return &words[otherIdx];
  }
  return nullptr;
}

int WordSelectNavigator::getCurrentFlatIndex() const {
  if (rows.empty() || currentRow >= static_cast<int>(rows.size())) return -1;
  if (rows[currentRow].wordIndices.empty()) return -1;
  return rows[currentRow].wordIndices[currentWordInRow];
}

const WordSelectNavigator::WordInfo* WordSelectNavigator::getWordAt(int idx) const {
  if (idx < 0 || idx >= static_cast<int>(words.size())) return nullptr;
  return &words[idx];
}

std::string WordSelectNavigator::buildPhrase(int fromIdx, int toIdx) const {
  const int lo = std::min(fromIdx, toIdx);
  const int hi = std::max(fromIdx, toIdx);
  std::string phrase;
  // Skip index for a hyphenated pair's second half once its merged lookup text
  // has already been emitted via the first half, so the pair isn't duplicated.
  int skipIdx = -1;
  for (int i = lo; i <= hi; i++) {
    if (i == skipIdx) continue;
    const auto* w = getWordAt(i);
    if (!w) continue;
    if (!phrase.empty()) phrase += ' ';
    // getLookup() returns the merged, hyphen-stripped text for a hyphenated
    // pair (e.g. "externity" for "exter-" + "nity"), matching the single-word
    // lookup path. For ordinary words it equals the display text.
    phrase += getLookup(*w);
    if (w->continuationIndex >= 0) skipIdx = w->continuationIndex;
  }
  return phrase;
}

int WordSelectNavigator::findClosestWord(int targetRow) const {
  if (rows[targetRow].wordIndices.empty()) return 0;
  const int wordIdx = rows[currentRow].wordIndices[currentWordInRow];
  const int currentCenterX = words[wordIdx].screenX + words[wordIdx].width / 2;
  return findClosestWordFromX(targetRow, currentCenterX);
}

int WordSelectNavigator::findClosestWordFromX(int targetRow, int refCenterX) const {
  if (rows[targetRow].wordIndices.empty()) return 0;
  int bestMatch = 0;
  int bestDist = INT_MAX;
  for (int i = 0; i < static_cast<int>(rows[targetRow].wordIndices.size()); i++) {
    const int idx = rows[targetRow].wordIndices[i];
    const int centerX = words[idx].screenX + words[idx].width / 2;
    const int dist = std::abs(centerX - refCenterX);
    if (dist < bestDist) {
      bestDist = dist;
      bestMatch = i;
    }
  }
  return bestMatch;
}

bool WordSelectNavigator::handleNavigation(const MappedInputManager& input, const GfxRenderer& renderer) {
  if (rows.empty()) return false;

  const auto orient = renderer.getOrientation();
  const bool isLandscapeCw = orient == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orient == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orient == GfxRenderer::Orientation::PortraitInverted;
  const bool landscape = isLandscapeCw || isLandscapeCcw;

  bool rowPrevPressed, rowNextPressed, wordPrevPressed, wordNextPressed;

  if (isLandscapeCw) {
    rowPrevPressed = input.wasReleased(MappedInputManager::Button::Left);
    rowNextPressed = input.wasReleased(MappedInputManager::Button::Right);
    wordPrevPressed = input.wasReleased(MappedInputManager::Button::Down);
    wordNextPressed = input.wasReleased(MappedInputManager::Button::Up);
  } else if (landscape) {
    rowPrevPressed = input.wasReleased(MappedInputManager::Button::Right);
    rowNextPressed = input.wasReleased(MappedInputManager::Button::Left);
    wordPrevPressed = input.wasReleased(MappedInputManager::Button::Up);
    wordNextPressed = input.wasReleased(MappedInputManager::Button::Down);
  } else if (isInverted) {
    rowPrevPressed = input.wasReleased(MappedInputManager::Button::Down);
    rowNextPressed = input.wasReleased(MappedInputManager::Button::Up);
    wordPrevPressed = input.wasReleased(MappedInputManager::Button::Right);
    wordNextPressed = input.wasReleased(MappedInputManager::Button::Left);
  } else {
    rowPrevPressed = input.wasReleased(MappedInputManager::Button::Up);
    rowNextPressed = input.wasReleased(MappedInputManager::Button::Down);
    wordPrevPressed = input.wasReleased(MappedInputManager::Button::Left);
    wordNextPressed = input.wasReleased(MappedInputManager::Button::Right);
  }

  const int rowCount = static_cast<int>(rows.size());
  bool changed = false;
  const int prevFlatIdx = getCurrentFlatIndex();
  const int currentCenterX = (prevFlatIdx >= 0) ? words[prevFlatIdx].screenX + words[prevFlatIdx].width / 2 : -1;

  // If the previous action was a wordPrev snap (second half → first half across
  // rows), use the second half's position as the row-nav reference so that
  // rowPrev/rowNext feels like it originates from where the user was.
  // Any directional input clears this state.
  const bool hasPendingSnap = pendingSnapIdx >= 0;
  const int pendingSnapRow = hasPendingSnap ? words[pendingSnapIdx].row : -1;
  const int pendingSnapRefX = hasPendingSnap ? words[pendingSnapIdx].screenX + words[pendingSnapIdx].width / 2 : -1;
  const int rowNavBase = hasPendingSnap ? pendingSnapRow : currentRow;
  if (rowPrevPressed || rowNextPressed || wordPrevPressed || wordNextPressed) {
    pendingSnapIdx = -1;
  }

  if (rowPrevPressed) {
    const int rowNavRefX =
        hasPendingSnap ? pendingSnapRefX : (preferredRowNavX >= 0 ? preferredRowNavX : currentCenterX);
    const int targetRow = (rowNavBase > 0) ? rowNavBase - 1 : rowCount - 1;
    currentWordInRow = (rowNavRefX >= 0) ? findClosestWordFromX(targetRow, rowNavRefX) : findClosestWord(targetRow);
    currentRow = targetRow;
    preferredRowNavX = rowNavRefX;
    changed = true;
  }

  if (rowNextPressed) {
    const int rowNavRefX =
        hasPendingSnap ? pendingSnapRefX : (preferredRowNavX >= 0 ? preferredRowNavX : currentCenterX);
    const int targetRow = (rowNavBase < rowCount - 1) ? rowNavBase + 1 : 0;
    currentWordInRow = (rowNavRefX >= 0) ? findClosestWordFromX(targetRow, rowNavRefX) : findClosestWord(targetRow);
    currentRow = targetRow;
    preferredRowNavX = rowNavRefX;
    changed = true;
  }

  if (wordPrevPressed) {
    preferredRowNavX = -1;
    if (currentWordInRow > 0) {
      currentWordInRow--;
    } else if (rowCount > 1) {
      currentRow = (currentRow > 0) ? currentRow - 1 : rowCount - 1;
      currentWordInRow = static_cast<int>(rows[currentRow].wordIndices.size()) - 1;
    }
    changed = true;
  }

  if (wordNextPressed) {
    preferredRowNavX = -1;
    if (currentWordInRow < static_cast<int>(rows[currentRow].wordIndices.size()) - 1) {
      currentWordInRow++;
    } else if (rowCount > 1) {
      currentRow = (currentRow < rowCount - 1) ? currentRow + 1 : 0;
      currentWordInRow = 0;
    } else {
      currentWordInRow = 0;  // single-row wrap
    }
    changed = true;
  }

  // Hyphenated pair smoothing for horizontal navigation:
  // the second half should not be a horizontal stop since both halves
  // highlight together. Row navigation (up/down) is exempt — the user
  // may intend to land on the second half's row.
  if (changed) {
    const int idx = getCurrentFlatIndex();
    if (idx >= 0 && words[idx].continuationOf >= 0) {
      if (wordNextPressed) {
        // Moving forward: skip past the second half to the next word.
        if (currentWordInRow < static_cast<int>(rows[currentRow].wordIndices.size()) - 1) {
          currentWordInRow++;
        } else if (rowCount > 1) {
          currentRow = (currentRow < rowCount - 1) ? currentRow + 1 : 0;
          currentWordInRow = 0;
        } else {
          currentWordInRow = 0;  // single-row wrap
        }
        // If the skip landed on yet another continuation, snap to its first half.
        const int skippedIdx = getCurrentFlatIndex();
        if (skippedIdx >= 0 && words[skippedIdx].continuationOf >= 0) {
          const int firstIdx = words[skippedIdx].continuationOf;
          currentRow = words[firstIdx].row;
          for (int i = 0; i < static_cast<int>(rows[currentRow].wordIndices.size()); i++) {
            if (rows[currentRow].wordIndices[i] == firstIdx) {
              currentWordInRow = i;
              break;
            }
          }
        }
      } else if (wordPrevPressed) {
        // Moving backward: snap to the first half.
        // Record the second half's index so subsequent row navigation
        // references its position rather than the first half's.
        pendingSnapIdx = idx;
        const int firstIdx = words[idx].continuationOf;
        currentRow = words[firstIdx].row;
        for (int i = 0; i < static_cast<int>(rows[currentRow].wordIndices.size()); i++) {
          if (rows[currentRow].wordIndices[i] == firstIdx) {
            currentWordInRow = i;
            break;
          }
        }
      }
      // Row navigation leaves cursor on whichever half
      // findClosestWord landed on. Both halves highlight regardless.
    }

    // Symmetric with the wordNext skip: if we came directly from the second
    // half and wrapped into its first half, skip backward past the first half
    // so the pair is treated as a single navigation unit in both directions.
    if (wordPrevPressed) {
      const int curIdx = getCurrentFlatIndex();
      if (curIdx >= 0 && words[curIdx].continuationOf < 0 && words[curIdx].continuationIndex >= 0 &&
          prevFlatIdx == words[curIdx].continuationIndex) {
        if (currentWordInRow > 0) {
          currentWordInRow--;
        } else if (rowCount > 1) {
          currentRow = (currentRow > 0) ? currentRow - 1 : rowCount - 1;
          currentWordInRow = static_cast<int>(rows[currentRow].wordIndices.size()) - 1;
        }
      }
    }
  }

  return changed;
}

WordSelectNavigator::MultiSelectAction WordSelectNavigator::handleMultiSelectInput(const MappedInputManager& input,
                                                                                   std::string& outPhrase,
                                                                                   unsigned long longPressMs) {
  if (inMultiSelectMode) {
    // Consume the Confirm release that follows the threshold-fire entry into multi-select.
    if (confirmReleaseConsumed) {
      if (input.wasReleased(MappedInputManager::Button::Confirm)) {
        confirmReleaseConsumed = false;
      }
      return MultiSelectAction::None;
    }
    if (input.wasReleased(MappedInputManager::Button::Confirm)) {
      const int cursorIdx = getCurrentFlatIndex();
      outPhrase = buildPhrase(anchorFlatIndex, cursorIdx);
      inMultiSelectMode = false;
      return MultiSelectAction::PhraseReady;
    }
    if (input.wasReleased(MappedInputManager::Button::Back)) {
      inMultiSelectMode = false;
      return MultiSelectAction::ExitedMultiSelect;
    }
    return MultiSelectAction::None;
  }

  // Consume the Confirm press+release that carried over from the long-press that opened word selection.
  // Must block both the held-state check (which would immediately enter multi-select) and
  // the subsequent release event (which would trigger a single-word lookup in the activity).
  if (confirmReleaseConsumed) {
    if (input.wasReleased(MappedInputManager::Button::Confirm)) {
      confirmReleaseConsumed = false;
    }
    return MultiSelectAction::Consumed;
  }

  // Long press Confirm: enter multi-select (fire at threshold, not on release).
  if (input.isPressed(MappedInputManager::Button::Confirm) && input.getHeldTime() >= longPressMs) {
    const int flatIdx = getCurrentFlatIndex();
    if (flatIdx >= 0) {
      inMultiSelectMode = true;
      anchorFlatIndex = flatIdx;
      confirmReleaseConsumed = true;
      return MultiSelectAction::EnteredMultiSelect;
    }
    return MultiSelectAction::Consumed;
  }

  return MultiSelectAction::None;
}

void WordSelectNavigator::renderHighlight(const GfxRenderer& renderer, int lineHeight) const {
  if (inMultiSelectMode) {
    const int cursorIdx = getCurrentFlatIndex();
    const int lo = std::min(anchorFlatIndex, cursorIdx);
    const int hi = std::max(anchorFlatIndex, cursorIdx);
    for (int i = lo; i <= hi; i++) {
      drawSingleHighlight(renderer, lineHeight, i);
      drawContinuationsIfOutside(renderer, lineHeight, getWordAt(i), lo, hi);
    }
  } else {
    const int selIdx = getCurrentFlatIndex();
    if (selIdx < 0) return;
    drawSingleHighlight(renderer, lineHeight, selIdx);
    drawContinuationsIfOutside(renderer, lineHeight, getWordAt(selIdx), selIdx, selIdx);
  }
}

void WordSelectNavigator::drawSingleHighlight(const GfxRenderer& renderer, int lineHeight, int wordIndex) const {
  const auto* w = getWordAt(wordIndex);
  if (!w) return;
  renderer.fillRect(w->screenX - 2, w->screenY - 2, w->width + 4, lineHeight + 4, true);
  renderer.drawText(w->fontId, w->screenX, w->screenY, getDisplay(*w), false, w->style);
}

void WordSelectNavigator::drawContinuationsIfOutside(const GfxRenderer& renderer, int lineHeight, const WordInfo* w,
                                                     int lo, int hi) const {
  if (!w) return;
  if (w->continuationIndex >= 0 && (w->continuationIndex < lo || w->continuationIndex > hi)) {
    drawSingleHighlight(renderer, lineHeight, w->continuationIndex);
  }
  if (w->continuationOf >= 0 && (w->continuationOf < lo || w->continuationOf > hi)) {
    drawSingleHighlight(renderer, lineHeight, w->continuationOf);
  }
}
