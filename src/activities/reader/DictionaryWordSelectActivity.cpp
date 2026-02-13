#include "DictionaryWordSelectActivity.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <climits>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "DictionarySuggestionsActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "util/Dictionary.h"
#include "util/LookupHistory.h"

void DictionaryWordSelectActivity::taskTrampoline(void* param) {
  auto* self = static_cast<DictionaryWordSelectActivity*>(param);
  self->displayTaskLoop();
}

void DictionaryWordSelectActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderScreen();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void DictionaryWordSelectActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  extractWords();
  mergeHyphenatedWords();
  if (!rows.empty()) {
    currentRow = static_cast<int>(rows.size()) / 3;
    currentWordInRow = 0;
  }
  updateRequired = true;
  xTaskCreate(&DictionaryWordSelectActivity::taskTrampoline, "DictWordSelTask", 4096, this, 1, &displayTaskHandle);
}

void DictionaryWordSelectActivity::onExit() {
  ActivityWithSubactivity::onExit();
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

bool DictionaryWordSelectActivity::isInverted() const {
  return orientation == CrossPointSettings::ORIENTATION::INVERTED;
}

void DictionaryWordSelectActivity::extractWords() {
  words.clear();
  rows.clear();

  for (const auto& element : page->elements) {
    // PageLine is the only concrete PageElement type, identified by tag
    const auto* line = static_cast<const PageLine*>(element.get());

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

    // Set bidirectional continuation links for highlighting both parts
    words[lastWordIdx].continuationIndex = nextWordIdx;
    words[nextWordIdx].continuationOf = lastWordIdx;

    // Build merged lookup text: remove trailing hyphen and combine
    std::string firstPart = lastWord;
    if (firstPart.back() == '-') {
      firstPart.pop_back();
    } else if (firstPart.size() >= 2 && static_cast<uint8_t>(firstPart[firstPart.size() - 2]) == 0xC2 &&
               static_cast<uint8_t>(firstPart[firstPart.size() - 1]) == 0xAD) {
      firstPart.erase(firstPart.size() - 2);
    }
    std::string merged = firstPart + words[nextWordIdx].text;
    words[lastWordIdx].lookupText = merged;
    words[nextWordIdx].lookupText = merged;
    words[nextWordIdx].continuationIndex = nextWordIdx;  // self-ref so highlight logic finds the second part
  }

  // Remove empty rows that may result from merging (e.g., a row whose only word was a continuation)
  rows.erase(std::remove_if(rows.begin(), rows.end(), [](const Row& r) { return r.wordIndices.empty(); }), rows.end());
}

void DictionaryWordSelectActivity::loop() {
  // Delegate to subactivity (definition screen) if active
  if (subActivity) {
    subActivity->loop();
    if (pendingBackFromDef) {
      pendingBackFromDef = false;
      exitActivity();
      updateRequired = true;
    }
    if (pendingExitToReader) {
      pendingExitToReader = false;
      exitActivity();
      onBack();
    }
    return;
  }

  if (words.empty()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onBack();
    }
    return;
  }

  bool changed = false;
  const bool landscape = isLandscape();
  const bool inverted = isInverted();

  // Button mapping depends on physical orientation:
  // - Portrait: side Up/Down = row nav, face Left/Right = word nav
  // - Inverted: same axes but reversed directions (device is flipped 180)
  // - Landscape: face Left/Right = row nav (swapped), side Up/Down = word nav
  bool rowPrevPressed, rowNextPressed, wordPrevPressed, wordNextPressed;

  if (landscape && orientation == CrossPointSettings::ORIENTATION::LANDSCAPE_CW) {
    rowPrevPressed = mappedInput.wasReleased(MappedInputManager::Button::Left);
    rowNextPressed = mappedInput.wasReleased(MappedInputManager::Button::Right);
    wordPrevPressed = mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
                      mappedInput.wasReleased(MappedInputManager::Button::Down);
    wordNextPressed = mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                      mappedInput.wasReleased(MappedInputManager::Button::Up);
  } else if (landscape) {
    rowPrevPressed = mappedInput.wasReleased(MappedInputManager::Button::Right);
    rowNextPressed = mappedInput.wasReleased(MappedInputManager::Button::Left);
    wordPrevPressed = mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                      mappedInput.wasReleased(MappedInputManager::Button::Up);
    wordNextPressed = mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
                      mappedInput.wasReleased(MappedInputManager::Button::Down);
  } else if (inverted) {
    rowPrevPressed = mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
                     mappedInput.wasReleased(MappedInputManager::Button::Down);
    rowNextPressed = mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                     mappedInput.wasReleased(MappedInputManager::Button::Up);
    wordPrevPressed = mappedInput.wasReleased(MappedInputManager::Button::Right);
    wordNextPressed = mappedInput.wasReleased(MappedInputManager::Button::Left);
  } else {
    // Portrait (default)
    rowPrevPressed = mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                     mappedInput.wasReleased(MappedInputManager::Button::Up);
    rowNextPressed = mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
                     mappedInput.wasReleased(MappedInputManager::Button::Down);
    wordPrevPressed = mappedInput.wasReleased(MappedInputManager::Button::Left);
    wordNextPressed = mappedInput.wasReleased(MappedInputManager::Button::Right);
  }

  const int rowCount = static_cast<int>(rows.size());

  // Helper: find closest word by X position in a target row
  auto findClosestWord = [&](int targetRow) {
    int wordIdx = rows[currentRow].wordIndices[currentWordInRow];
    int currentCenterX = words[wordIdx].screenX + words[wordIdx].width / 2;
    int bestMatch = 0;
    int bestDist = INT_MAX;
    for (int i = 0; i < static_cast<int>(rows[targetRow].wordIndices.size()); i++) {
      int idx = rows[targetRow].wordIndices[i];
      int centerX = words[idx].screenX + words[idx].width / 2;
      int dist = std::abs(centerX - currentCenterX);
      if (dist < bestDist) {
        bestDist = dist;
        bestMatch = i;
      }
    }
    return bestMatch;
  };

  // Move to previous row (wrap to bottom)
  if (rowPrevPressed) {
    int targetRow = (currentRow > 0) ? currentRow - 1 : rowCount - 1;
    currentWordInRow = findClosestWord(targetRow);
    currentRow = targetRow;
    changed = true;
  }

  // Move to next row (wrap to top)
  if (rowNextPressed) {
    int targetRow = (currentRow < rowCount - 1) ? currentRow + 1 : 0;
    currentWordInRow = findClosestWord(targetRow);
    currentRow = targetRow;
    changed = true;
  }

  // Move to previous word (wrap to end of previous row)
  if (wordPrevPressed) {
    if (currentWordInRow > 0) {
      currentWordInRow--;
    } else if (rowCount > 1) {
      currentRow = (currentRow > 0) ? currentRow - 1 : rowCount - 1;
      currentWordInRow = static_cast<int>(rows[currentRow].wordIndices.size()) - 1;
    }
    changed = true;
  }

  // Move to next word (wrap to start of next row)
  if (wordNextPressed) {
    if (currentWordInRow < static_cast<int>(rows[currentRow].wordIndices.size()) - 1) {
      currentWordInRow++;
    } else if (rowCount > 1) {
      currentRow = (currentRow < rowCount - 1) ? currentRow + 1 : 0;
      currentWordInRow = 0;
    }
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

    // Show looking up popup, then release mutex so display task can run
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    Rect popupLayout = GUI.drawPopup(renderer, "Looking up...");
    xSemaphoreGive(renderingMutex);

    bool cancelled = false;
    std::string definition = Dictionary::lookup(
        cleaned,
        [this, &popupLayout](int percent) {
          xSemaphoreTake(renderingMutex, portMAX_DELAY);
          GUI.fillPopupProgress(renderer, popupLayout, percent);
          xSemaphoreGive(renderingMutex);
        },
        [this, &cancelled]() -> bool {
          mappedInput.update();
          if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
            cancelled = true;
            return true;
          }
          return false;
        });

    if (cancelled) {
      updateRequired = true;
      return;
    }

    if (!definition.empty()) {
      LookupHistory::addWord(cachePath, cleaned);
      enterNewActivity(new DictionaryDefinitionActivity(
          renderer, mappedInput, cleaned, definition, fontId, orientation, [this]() { pendingBackFromDef = true; },
          [this]() { pendingExitToReader = true; }));
      return;
    }

    // Try stem variants (e.g., "jumped" → "jump")
    auto stems = Dictionary::getStemVariants(cleaned);
    for (const auto& stem : stems) {
      std::string stemDef = Dictionary::lookup(stem);
      if (!stemDef.empty()) {
        LookupHistory::addWord(cachePath, stem);
        enterNewActivity(new DictionaryDefinitionActivity(
            renderer, mappedInput, stem, stemDef, fontId, orientation, [this]() { pendingBackFromDef = true; },
            [this]() { pendingExitToReader = true; }));
        return;
      }
    }

    // Find similar words for suggestions
    auto similar = Dictionary::findSimilar(cleaned, 6);
    if (!similar.empty()) {
      enterNewActivity(new DictionarySuggestionsActivity(
          renderer, mappedInput, cleaned, similar, fontId, cachePath, orientation,
          [this]() { pendingBackFromDef = true; }, [this]() { pendingExitToReader = true; }));
      return;
    }

    GUI.drawPopup(renderer, "Not found");
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    vTaskDelay(1500 / portTICK_PERIOD_MS);
    updateRequired = true;
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

    // Highlight the other half of a hyphenated word (whether selecting first or second part)
    int otherIdx = (w.continuationOf >= 0) ? w.continuationOf : -1;
    if (otherIdx < 0 && w.continuationIndex >= 0 && w.continuationIndex != wordIdx) {
      otherIdx = w.continuationIndex;
    }
    if (otherIdx >= 0) {
      const auto& other = words[otherIdx];
      renderer.fillRect(other.screenX - 1, other.screenY - 1, other.width + 2, lineHeight + 2, true);
      renderer.drawText(fontId, other.screenX, other.screenY, other.text.c_str(), false);
    }
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
