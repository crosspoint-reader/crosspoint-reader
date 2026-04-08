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

void WordSelectNavigator::renderHighlight(const GfxRenderer& renderer, int lineHeight) const {
  if (inMultiSelectMode) {
    const int cursorIdx = getCurrentFlatIndex();
    const int lo = std::min(anchorFlatIndex, cursorIdx);
    const int hi = std::max(anchorFlatIndex, cursorIdx);
    for (int i = lo; i <= hi; i++) {
      const auto* w = getWordAt(i);
      if (!w) continue;
      renderer.fillRect(w->screenX - 2, w->screenY - 2, w->width + 4, lineHeight + 4, true);
      renderer.drawText(w->fontId, w->screenX, w->screenY, getDisplay(*w), false, w->style);
    }
  } else {
    const auto* sel = getSelected();
    if (!sel) return;
    renderer.fillRect(sel->screenX - 2, sel->screenY - 2, sel->width + 4, lineHeight + 4, true);
    renderer.drawText(sel->fontId, sel->screenX, sel->screenY, getDisplay(*sel), false, sel->style);
    const auto* other = getContinuation();
    if (other) {
      renderer.fillRect(other->screenX - 2, other->screenY - 2, other->width + 4, lineHeight + 4, true);
      renderer.drawText(other->fontId, other->screenX, other->screenY, getDisplay(*other), false, other->style);
    }
  }
}
