#include "WordSelectNavigator.h"

#include <GfxRenderer.h>
#include <Utf8.h>

#include <cstdlib>

#include "MappedInputManager.h"
#include "TextPool.h"

void WordSelectNavigator::load(std::vector<WordInfo> w, std::vector<Row> r, std::string pool,
                               bool consumeInitialConfirm) {
  words = std::move(w);
  rows = std::move(r);
  textPool = std::move(pool);
  currentRow = static_cast<int>(rows.size()) / 2;
  currentWordInRow = (!rows.empty() && !rows[currentRow].wordIndices.empty())
                         ? static_cast<int>(rows[currentRow].wordIndices.size()) / 2
                         : 0;
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
  for (int i = lo; i <= hi; i++) {
    const auto* w = getWordAt(i);
    if (!w) continue;
    if (!phrase.empty()) phrase += ' ';
    phrase += getDisplay(*w);
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

  // If the previous action was a wordPrev snap (second half → first half across
  // rows), use the second half's position as the row-nav reference so that
  // rowPrev/rowNext feels like it originates from where the user was.
  // Any directional input clears this state.
  const bool hasPendingSnap = pendingSnapIdx >= 0;
  const int rowNavBase = hasPendingSnap ? words[pendingSnapIdx].row : currentRow;
  const int rowNavRefX = hasPendingSnap ? words[pendingSnapIdx].screenX + words[pendingSnapIdx].width / 2 : -1;
  if (rowPrevPressed || rowNextPressed || wordPrevPressed || wordNextPressed) {
    pendingSnapIdx = -1;
  }

  if (rowPrevPressed) {
    const int targetRow = (rowNavBase > 0) ? rowNavBase - 1 : rowCount - 1;
    currentWordInRow = (rowNavRefX >= 0) ? findClosestWordFromX(targetRow, rowNavRefX) : findClosestWord(targetRow);
    currentRow = targetRow;
    changed = true;
  }

  if (rowNextPressed) {
    const int targetRow = (rowNavBase < rowCount - 1) ? rowNavBase + 1 : 0;
    currentWordInRow = (rowNavRefX >= 0) ? findClosestWordFromX(targetRow, rowNavRefX) : findClosestWord(targetRow);
    currentRow = targetRow;
    changed = true;
  }

  if (wordPrevPressed) {
    if (currentWordInRow > 0) {
      currentWordInRow--;
    } else if (rowCount > 1) {
      currentRow = (currentRow > 0) ? currentRow - 1 : rowCount - 1;
      currentWordInRow = static_cast<int>(rows[currentRow].wordIndices.size()) - 1;
    }
    changed = true;
  }

  if (wordNextPressed) {
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

bool WordSelectNavigator::HighlightSnapshot::capture(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                                     const GfxRenderer& renderer) {
  if (w == 0 || h == 0) {
    bytes_ = 0;
    return false;
  }
  // The renderer translates (x, y, w, h) from screen to byte-aligned memory
  // coords and writes that many bytes; it returns 0 on capacity overflow,
  // out-of-bounds, or rejection. We do NOT pre-check capacity here because the
  // aligned-memory size differs from the naive screen-coord size, and only
  // the renderer knows the exact figure.
  const size_t written = renderer.readFramebufferRegion(x, y, w, h, buf_, MAX_SNAPSHOT_BYTES);
  if (written == 0) {
    bytes_ = 0;
    return false;
  }
  x_ = x;
  y_ = y;
  w_ = w;
  h_ = h;
  bytes_ = written;
  return true;
}

void WordSelectNavigator::HighlightSnapshot::restore(GfxRenderer& renderer) const {
  if (bytes_ == 0) return;
  renderer.writeFramebufferRegion(x_, y_, w_, h_, buf_);
}

void WordSelectNavigator::renderHighlight(const GfxRenderer& renderer, int lineHeight) const {
  if (inMultiSelectMode) {
    const int cursorIdx = getCurrentFlatIndex();
    const int lo = std::min(anchorFlatIndex, cursorIdx);
    const int hi = std::max(anchorFlatIndex, cursorIdx);
    for (int i = lo; i <= hi; i++) {
      drawSingleHighlight(renderer, lineHeight, i);
    }
  } else {
    const int selIdx = getCurrentFlatIndex();
    if (selIdx < 0) return;
    drawSingleHighlight(renderer, lineHeight, selIdx);
    const auto* sel = getWordAt(selIdx);
    if (sel && sel->continuationIndex >= 0) {
      drawSingleHighlight(renderer, lineHeight, sel->continuationIndex);
    }
    if (sel && sel->continuationOf >= 0) {
      drawSingleHighlight(renderer, lineHeight, sel->continuationOf);
    }
  }
}

void WordSelectNavigator::drawSingleHighlight(const GfxRenderer& renderer, int lineHeight, int wordIndex) const {
  const auto* w = getWordAt(wordIndex);
  if (!w) return;
  renderer.fillRect(w->screenX - 2, w->screenY - 2, w->width + 4, lineHeight + 4, true);
  renderer.drawText(w->fontId, w->screenX, w->screenY, getDisplay(*w), false, w->style);
}

WordSelectNavigator::Rect WordSelectNavigator::boundsForWord(int wordIndex, int lineHeight) const {
  const auto* w = getWordAt(wordIndex);
  if (!w) return Rect{};
  return Rect{static_cast<int>(w->screenX) - 2, static_cast<int>(w->screenY) - 2, static_cast<int>(w->width) + 4,
              lineHeight + 4};
}

WordSelectNavigator::Rect WordSelectNavigator::computeDirtyRect(int prevWordIdx, int currWordIdx,
                                                                int lineHeight) const {
  Rect curr = boundsForWord(currWordIdx, lineHeight);
  if (prevWordIdx < 0) return curr;
  Rect prev = boundsForWord(prevWordIdx, lineHeight);
  if (prev.width == 0 || prev.height == 0) return curr;
  if (curr.width == 0 || curr.height == 0) return prev;
  const int x0 = std::min(prev.x, curr.x);
  const int y0 = std::min(prev.y, curr.y);
  const int x1 = std::max(prev.x + prev.width, curr.x + curr.width);
  const int y1 = std::max(prev.y + prev.height, curr.y + curr.height);
  return Rect{x0, y0, x1 - x0, y1 - y0};
}

std::optional<WordSelectNavigator::Rect> WordSelectNavigator::renderHighlightDifferential(GfxRenderer& renderer,
                                                                                          int lineHeight,
                                                                                          int prevWordIdx,
                                                                                          int currWordIdx) {
  // Fallback paths.
  if (inMultiSelectMode) return std::nullopt;
  const auto* curr = getWordAt(currWordIdx);
  if (!curr) return std::nullopt;
  if (curr->continuationIndex >= 0 || curr->continuationOf >= 0) {
    // Hyphenated wrap — two-word highlight is not yet supported by the
    // single-snapshot fast path. Caller falls back to full repaint.
    return std::nullopt;
  }

  // Step 1: restore pixels under the previous highlight (wipe it).
  // prevWordIdx < 0 is the caller's signal "no previous highlight on screen"
  // (typically because the framebuffer was just redrawn from scratch via the
  // full-repaint path or a sub-activity return). In that case any snapshot we
  // still hold from a prior render cycle is stale relative to the current
  // framebuffer — discard it rather than restoring it on top of fresh pixels.
  if (prevWordIdx >= 0 && snapshot_.valid()) {
    snapshot_.restore(renderer);
  }
  snapshot_.clear();

  // Step 2: snapshot pixels under the new highlight, clamping coordinates so we
  // never pass negative values into the renderer's uint16_t API.
  const Rect newRect = boundsForWord(currWordIdx, lineHeight);
  const uint16_t snapX = static_cast<uint16_t>(std::max(newRect.x, 0));
  const uint16_t snapY = static_cast<uint16_t>(std::max(newRect.y, 0));
  const uint16_t snapW = static_cast<uint16_t>(std::max(newRect.width, 0));
  const uint16_t snapH = static_cast<uint16_t>(std::max(newRect.height, 0));
  if (!snapshot_.capture(snapX, snapY, snapW, snapH, renderer)) {
    // Capture failed — either too big or out of bounds. Caller falls back.
    return std::nullopt;
  }

  // Step 3: draw the new highlight on top of the captured pixels.
  drawSingleHighlight(renderer, lineHeight, currWordIdx);

  // Step 4: caller pushes the union region.
  return computeDirtyRect(prevWordIdx, currWordIdx, lineHeight);
}
