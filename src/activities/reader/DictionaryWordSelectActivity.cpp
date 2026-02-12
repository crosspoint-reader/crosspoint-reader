#include "DictionaryWordSelectActivity.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <climits>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/LookupHistory.h"

void DictionaryWordSelectActivity::taskTrampoline(void* param) {
  auto* self = static_cast<DictionaryWordSelectActivity*>(param);
  self->displayTaskLoop();
}

void DictionaryWordSelectActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderScreen();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void DictionaryWordSelectActivity::onEnter() {
  Activity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  extractWords();
  mergeHyphenatedWords();
  updateRequired = true;
  xTaskCreate(&DictionaryWordSelectActivity::taskTrampoline, "DictWordSelTask", 4096, this, 1, &displayTaskHandle);
}

void DictionaryWordSelectActivity::onExit() {
  Activity::onExit();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

bool DictionaryWordSelectActivity::isLandscape() const {
  return orientation == CrossPointSettings::ORIENTATION::LANDSCAPE_CW ||
         orientation == CrossPointSettings::ORIENTATION::LANDSCAPE_CCW;
}

void DictionaryWordSelectActivity::extractWords() {
  words.clear();
  rows.clear();

  for (const auto& element : page->elements) {
    // PageLine is the only concrete PageElement type, identified by tag
    auto* line = static_cast<PageLine*>(element.get());

    const auto& block = line->getBlock();
    if (!block) continue;

    const auto& wordList = block->getWords();
    const auto& xPosList = block->getWordXpos();

    auto wordIt = wordList.begin();
    auto xIt = xPosList.begin();

    while (wordIt != wordList.end() && xIt != xPosList.end()) {
      int16_t screenX = line->xPos + static_cast<int16_t>(*xIt) + marginLeft;
      int16_t screenY = line->yPos + marginTop;
      int16_t wordWidth = renderer.getTextWidth(fontId, wordIt->c_str());

      words.push_back({*wordIt, screenX, screenY, wordWidth, 0});
      ++wordIt;
      ++xIt;
    }
  }

  // Group words into rows by Y position
  if (words.empty()) return;

  int16_t currentY = words[0].screenY;
  rows.push_back({currentY, {}});

  for (size_t i = 0; i < words.size(); i++) {
    // Allow small Y tolerance (words on same line may differ by a pixel)
    if (std::abs(words[i].screenY - currentY) > 2) {
      currentY = words[i].screenY;
      rows.push_back({currentY, {}});
    }
    words[i].row = static_cast<int16_t>(rows.size() - 1);
    rows.back().wordIndices.push_back(static_cast<int>(i));
  }
}

void DictionaryWordSelectActivity::mergeHyphenatedWords() {
  for (size_t r = 0; r + 1 < rows.size(); r++) {
    if (rows[r].wordIndices.empty() || rows[r + 1].wordIndices.empty()) continue;

    int lastWordIdx = rows[r].wordIndices.back();
    const std::string& lastWord = words[lastWordIdx].text;
    if (lastWord.empty()) continue;

    // Check if word ends with hyphen (regular '-' or soft hyphen U+00AD: 0xC2 0xAD)
    bool endsWithHyphen = false;
    if (lastWord.back() == '-') {
      endsWithHyphen = true;
    } else if (lastWord.size() >= 2 && static_cast<uint8_t>(lastWord[lastWord.size() - 2]) == 0xC2 &&
               static_cast<uint8_t>(lastWord[lastWord.size() - 1]) == 0xAD) {
      endsWithHyphen = true;
    }

    if (!endsWithHyphen) continue;

    int nextWordIdx = rows[r + 1].wordIndices.front();

    // Set continuation link for highlighting both parts
    words[lastWordIdx].continuationIndex = nextWordIdx;

    // Build lookup text: remove trailing hyphen and combine with continuation
    std::string firstPart = lastWord;
    if (firstPart.back() == '-') {
      firstPart.pop_back();
    } else if (firstPart.size() >= 2 && static_cast<uint8_t>(firstPart[firstPart.size() - 2]) == 0xC2 &&
               static_cast<uint8_t>(firstPart[firstPart.size() - 1]) == 0xAD) {
      firstPart.erase(firstPart.size() - 2);
    }
    words[lastWordIdx].lookupText = firstPart + words[nextWordIdx].text;

    // Remove the continuation word from the next row so it's not independently selectable
    rows[r + 1].wordIndices.erase(rows[r + 1].wordIndices.begin());
  }

  // Remove empty rows that may result from merging (e.g., a row whose only word was a continuation)
  rows.erase(std::remove_if(rows.begin(), rows.end(), [](const Row& r) { return r.wordIndices.empty(); }),
             rows.end());
}

void DictionaryWordSelectActivity::loop() {
  if (words.empty()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onBack();
    }
    return;
  }

  bool changed = false;
  const bool landscape = isLandscape();

  // In landscape, swap axes: face Left/Right → row navigation, side Up/Down → word-within-row
  // In portrait: side Up/Down → row navigation, face Left/Right → word-within-row
  const bool rowPrevPressed =
      landscape ? mappedInput.wasReleased(MappedInputManager::Button::Left)
                : (mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                   mappedInput.wasReleased(MappedInputManager::Button::Up));
  const bool rowNextPressed =
      landscape ? mappedInput.wasReleased(MappedInputManager::Button::Right)
                : (mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
                   mappedInput.wasReleased(MappedInputManager::Button::Down));
  const bool wordPrevPressed =
      landscape ? (mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                   mappedInput.wasReleased(MappedInputManager::Button::Up))
                : mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool wordNextPressed =
      landscape ? (mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
                   mappedInput.wasReleased(MappedInputManager::Button::Down))
                : mappedInput.wasReleased(MappedInputManager::Button::Right);

  // Move to previous row (position-based: find word closest to current word's X position)
  if (rowPrevPressed && currentRow > 0) {
    int wordIdx = rows[currentRow].wordIndices[currentWordInRow];
    int currentCenterX = words[wordIdx].screenX + words[wordIdx].width / 2;

    currentRow--;

    int bestMatch = 0;
    int bestDist = INT_MAX;
    for (int i = 0; i < static_cast<int>(rows[currentRow].wordIndices.size()); i++) {
      int idx = rows[currentRow].wordIndices[i];
      int centerX = words[idx].screenX + words[idx].width / 2;
      int dist = std::abs(centerX - currentCenterX);
      if (dist < bestDist) {
        bestDist = dist;
        bestMatch = i;
      }
    }
    currentWordInRow = bestMatch;
    changed = true;
  }

  // Move to next row (position-based)
  if (rowNextPressed && currentRow < static_cast<int>(rows.size()) - 1) {
    int wordIdx = rows[currentRow].wordIndices[currentWordInRow];
    int currentCenterX = words[wordIdx].screenX + words[wordIdx].width / 2;

    currentRow++;

    int bestMatch = 0;
    int bestDist = INT_MAX;
    for (int i = 0; i < static_cast<int>(rows[currentRow].wordIndices.size()); i++) {
      int idx = rows[currentRow].wordIndices[i];
      int centerX = words[idx].screenX + words[idx].width / 2;
      int dist = std::abs(centerX - currentCenterX);
      if (dist < bestDist) {
        bestDist = dist;
        bestMatch = i;
      }
    }
    currentWordInRow = bestMatch;
    changed = true;
  }

  // Move to previous word in row
  if (wordPrevPressed && currentWordInRow > 0) {
    currentWordInRow--;
    changed = true;
  }

  // Move to next word in row
  if (wordNextPressed && currentWordInRow < static_cast<int>(rows[currentRow].wordIndices.size()) - 1) {
    currentWordInRow++;
    changed = true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    int wordIdx = rows[currentRow].wordIndices[currentWordInRow];
    const std::string& rawWord = words[wordIdx].lookupText;
    std::string cleaned = Dictionary::cleanWord(rawWord);

    if (cleaned.empty()) {
      GUI.drawPopup(renderer, "No word");
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      updateRequired = true;
      return;
    }

    // Show looking up popup with progress bar
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    Rect popupLayout = GUI.drawPopup(renderer, "Looking up...");

    std::string definition = Dictionary::lookup(cleaned, [this, &popupLayout](int percent) {
      GUI.fillPopupProgress(renderer, popupLayout, percent);
    });
    xSemaphoreGive(renderingMutex);

    if (definition.empty()) {
      GUI.drawPopup(renderer, "Not found");
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      vTaskDelay(1500 / portTICK_PERIOD_MS);
      updateRequired = true;
      return;
    }

    LookupHistory::addWord(cachePath, cleaned);
    onLookup(cleaned, definition);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  if (changed) {
    updateRequired = true;
  }
}

void DictionaryWordSelectActivity::renderScreen() {
  renderer.clearScreen();

  // Render the page content
  page->render(renderer, fontId, marginLeft, marginTop);

  if (!words.empty() && currentRow < static_cast<int>(rows.size())) {
    int wordIdx = rows[currentRow].wordIndices[currentWordInRow];
    const auto& w = words[wordIdx];

    // Draw inverted highlight behind selected word
    const int lineHeight = renderer.getLineHeight(fontId);
    renderer.fillRect(w.screenX - 1, w.screenY - 1, w.width + 2, lineHeight + 2, true);
    renderer.drawText(fontId, w.screenX, w.screenY, w.text.c_str(), false);

    // Also highlight continuation part if this is a hyphenated word
    if (w.continuationIndex >= 0) {
      const auto& cont = words[w.continuationIndex];
      renderer.fillRect(cont.screenX - 1, cont.screenY - 1, cont.width + 2, lineHeight + 2, true);
      renderer.drawText(fontId, cont.screenX, cont.screenY, cont.text.c_str(), false);
    }
  }

  // Draw side button hints in portrait coordinates to match physical button positions
  {
    const auto orig = renderer.getOrientation();
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);

    // Clear area behind side button hints to prevent text overlap
    const int sideHintX = renderer.getScreenWidth() - 4 - 30;  // matches drawSideButtonHints positioning
    renderer.fillRect(sideHintX - 2, 340, 36, 170, false);

    if (isLandscape()) {
      GUI.drawSideButtonHints(renderer, "left", "right");
    } else {
      GUI.drawSideButtonHints(renderer, "Up", "Down");
    }

    renderer.setOrientation(orig);
  }

  // Button hints (use plain text "left"/"right" instead of Unicode symbols that don't render)
  const bool landscape = isLandscape();
  const auto labels = landscape ? mappedInput.mapLabels("\xC2\xAB Back", "Lookup", "up", "down")
                                : mappedInput.mapLabels("\xC2\xAB Back", "Lookup", "left", "right");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
