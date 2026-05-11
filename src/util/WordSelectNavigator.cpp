#include "WordSelectNavigator.h"

#include <GfxRenderer.h>

#include <cstdlib>

#include "MappedInputManager.h"

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

uint16_t WordSelectNavigator::poolAppend(std::string& pool, const char* s, size_t len) {
  uint16_t offset = static_cast<uint16_t>(pool.size());
  if (pool.size() + len + 1 > pool.capacity()) pool.reserve(pool.capacity() + 256);
  pool.append(s, len);
  pool.push_back('\0');
  return offset;
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
}

const WordSelectNavigator::WordInfo* WordSelectNavigator::getSelected() const {
  if (rows.empty() || currentRow >= static_cast<int>(rows.size())) return nullptr;
  if (rows[currentRow].wordIndices.empty()) return nullptr;
  return &words[rows[currentRow].wordIndices[currentWordInRow]];
}

const WordSelectNavigator::WordInfo* WordSelectNavigator::getContinuation() const {
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
  int bestMatch = 0;
  int bestDist = INT_MAX;
  for (int i = 0; i < static_cast<int>(rows[targetRow].wordIndices.size()); i++) {
    const int idx = rows[targetRow].wordIndices[i];
    const int centerX = words[idx].screenX + words[idx].width / 2;
    const int dist = std::abs(centerX - currentCenterX);
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

  if (rowPrevPressed) {
    const int targetRow = (currentRow > 0) ? currentRow - 1 : rowCount - 1;
    currentWordInRow = findClosestWord(targetRow);
    currentRow = targetRow;
    changed = true;
  }

  if (rowNextPressed) {
    const int targetRow = (currentRow < rowCount - 1) ? currentRow + 1 : 0;
    currentWordInRow = findClosestWord(targetRow);
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
    }
    changed = true;
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
  if (curr->continuationIndex >= 0) {
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
