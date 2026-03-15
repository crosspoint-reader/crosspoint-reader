#include "DictionaryWordSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <climits>

#include "DictionaryDefinitionActivity.h"
#include "DictionarySuggestionsActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/LookupHistory.h"

void DictionaryWordSelectActivity::onEnter() {
  Activity::onEnter();
  extractWords();
  mergeHyphenatedWords();
  if (!rows.empty()) {
    currentRow = static_cast<int>(rows.size()) / 3;
    currentWordInRow = 0;
  }
  requestUpdate();
}

void DictionaryWordSelectActivity::onExit() { Activity::onExit(); }

void DictionaryWordSelectActivity::extractWords() {
  words.clear();
  rows.clear();

  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
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
      const std::string& wordText = *wordIt;

      // Split on en-dash (U+2013: E2 80 93) and em-dash (U+2014: E2 80 94)
      std::vector<size_t> splitStarts;
      size_t partStart = 0;
      for (size_t i = 0; i < wordText.size();) {
        if (i + 2 < wordText.size() && static_cast<uint8_t>(wordText[i]) == 0xE2 &&
            static_cast<uint8_t>(wordText[i + 1]) == 0x80 &&
            (static_cast<uint8_t>(wordText[i + 2]) == 0x93 || static_cast<uint8_t>(wordText[i + 2]) == 0x94)) {
          if (i > partStart) splitStarts.push_back(partStart);
          i += 3;
          partStart = i;
        } else {
          i++;
        }
      }
      if (partStart < wordText.size()) splitStarts.push_back(partStart);

      if (splitStarts.size() <= 1 && partStart == 0) {
        int16_t wordWidth = renderer.getTextWidth(fontId, wordText.c_str());
        words.push_back({wordText, screenX, screenY, wordWidth, 0});
      } else {
        for (size_t si = 0; si < splitStarts.size(); si++) {
          size_t start = splitStarts[si];
          size_t end = (si + 1 < splitStarts.size()) ? splitStarts[si + 1] : wordText.size();
          size_t textEnd = end;
          while (textEnd > start && textEnd <= wordText.size()) {
            if (textEnd >= 3 && static_cast<uint8_t>(wordText[textEnd - 3]) == 0xE2 &&
                static_cast<uint8_t>(wordText[textEnd - 2]) == 0x80 &&
                (static_cast<uint8_t>(wordText[textEnd - 1]) == 0x93 ||
                 static_cast<uint8_t>(wordText[textEnd - 1]) == 0x94)) {
              textEnd -= 3;
            } else {
              break;
            }
          }
          std::string part = wordText.substr(start, textEnd - start);
          if (part.empty()) continue;

          std::string prefix = wordText.substr(0, start);
          int16_t offsetX = prefix.empty() ? 0 : renderer.getTextWidth(fontId, prefix.c_str());
          int16_t partWidth = renderer.getTextWidth(fontId, part.c_str());
          words.push_back({part, static_cast<int16_t>(screenX + offsetX), screenY, partWidth, 0});
        }
      }

      ++wordIt;
      ++xIt;
    }
  }

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

void DictionaryWordSelectActivity::mergeHyphenatedWords() {
  for (size_t r = 0; r + 1 < rows.size(); r++) {
    if (rows[r].wordIndices.empty() || rows[r + 1].wordIndices.empty()) continue;

    int lastWordIdx = rows[r].wordIndices.back();
    const std::string& lastWord = words[lastWordIdx].text;
    if (lastWord.empty()) continue;

    bool endsWithHyphen = false;
    if (lastWord.back() == '-') {
      endsWithHyphen = true;
    } else if (lastWord.size() >= 2 && static_cast<uint8_t>(lastWord[lastWord.size() - 2]) == 0xC2 &&
               static_cast<uint8_t>(lastWord[lastWord.size() - 1]) == 0xAD) {
      endsWithHyphen = true;
    }
    if (!endsWithHyphen) continue;

    int nextWordIdx = rows[r + 1].wordIndices.front();
    words[lastWordIdx].continuationIndex = nextWordIdx;
    words[nextWordIdx].continuationOf = lastWordIdx;

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
    words[nextWordIdx].continuationIndex = nextWordIdx;
  }

  // Cross-page hyphenation
  if (!nextPageFirstWord.empty() && !rows.empty()) {
    int lastWordIdx = rows.back().wordIndices.back();
    const std::string& lastWord = words[lastWordIdx].text;
    if (!lastWord.empty()) {
      bool endsWithHyphen = false;
      if (lastWord.back() == '-') {
        endsWithHyphen = true;
      } else if (lastWord.size() >= 2 && static_cast<uint8_t>(lastWord[lastWord.size() - 2]) == 0xC2 &&
                 static_cast<uint8_t>(lastWord[lastWord.size() - 1]) == 0xAD) {
        endsWithHyphen = true;
      }
      if (endsWithHyphen) {
        std::string firstPart = lastWord;
        if (firstPart.back() == '-') {
          firstPart.pop_back();
        } else if (firstPart.size() >= 2 && static_cast<uint8_t>(firstPart[firstPart.size() - 2]) == 0xC2 &&
                   static_cast<uint8_t>(firstPart[firstPart.size() - 1]) == 0xAD) {
          firstPart.erase(firstPart.size() - 2);
        }
        words[lastWordIdx].lookupText = firstPart + nextPageFirstWord;
      }
    }
  }

  rows.erase(std::remove_if(rows.begin(), rows.end(), [](const Row& r) { return r.wordIndices.empty(); }), rows.end());
}

// Shared helper: run findSimilar for `word` and show suggestions or "not found" popup.
void DictionaryWordSelectActivity::handleNotFound(const std::string& word) {
  auto similar = Dictionary::findSimilar(word, 6);
  if (!similar.empty()) {
    startActivityForResult(
        std::make_unique<DictionarySuggestionsActivity>(renderer, mappedInput, word, similar, fontId, cachePath),
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            setResult(ActivityResult{});
            finish();
          } else {
            requestUpdate();
          }
        });
    return;
  }
  GUI.drawPopup(renderer, tr(STR_DICT_NOT_FOUND));
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  vTaskDelay(1500 / portTICK_PERIOD_MS);
  requestUpdate();
}

void DictionaryWordSelectActivity::loop() {
  if (words.empty()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      ActivityResult r;
      r.isCancelled = true;
      setResult(std::move(r));
      finish();
    }
    return;
  }

  // "Search synonyms?" prompt shown after all direct lookups fail
  if (isAskingSynonymSearch) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      isAskingSynonymSearch = false;
      std::string canonical = Dictionary::lookupSynonym(synSearchWord);
      if (!canonical.empty()) {
        std::string synDef = Dictionary::lookup(canonical);
        if (!synDef.empty()) {
          startActivityForResult(
              std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, canonical, synDef, fontId, true),
              [this](const ActivityResult& result) {
                if (!result.isCancelled) {
                  setResult(ActivityResult{});
                  finish();
                } else {
                  requestUpdate();
                }
              });
          return;
        }
      }
      // Synonym not found — fall through to findSimilar
      handleNotFound(synSearchWord);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      isAskingSynonymSearch = false;
      handleNotFound(synSearchWord);
      return;
    }
    return;  // Consume all other input while on the synonym prompt
  }

  bool changed = false;
  const auto orient = renderer.getOrientation();
  const bool isLandscapeCw = orient == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orient == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orient == GfxRenderer::Orientation::PortraitInverted;
  const bool landscape = isLandscapeCw || isLandscapeCcw;

  bool rowPrevPressed, rowNextPressed, wordPrevPressed, wordNextPressed;

  if (isLandscapeCw) {
    rowPrevPressed = mappedInput.wasReleased(MappedInputManager::Button::Left);
    rowNextPressed = mappedInput.wasReleased(MappedInputManager::Button::Right);
    wordPrevPressed = mappedInput.wasReleased(MappedInputManager::Button::Down);
    wordNextPressed = mappedInput.wasReleased(MappedInputManager::Button::Up);
  } else if (landscape) {
    rowPrevPressed = mappedInput.wasReleased(MappedInputManager::Button::Right);
    rowNextPressed = mappedInput.wasReleased(MappedInputManager::Button::Left);
    wordPrevPressed = mappedInput.wasReleased(MappedInputManager::Button::Up);
    wordNextPressed = mappedInput.wasReleased(MappedInputManager::Button::Down);
  } else if (isInverted) {
    rowPrevPressed = mappedInput.wasReleased(MappedInputManager::Button::Down);
    rowNextPressed = mappedInput.wasReleased(MappedInputManager::Button::Up);
    wordPrevPressed = mappedInput.wasReleased(MappedInputManager::Button::Right);
    wordNextPressed = mappedInput.wasReleased(MappedInputManager::Button::Left);
  } else {
    rowPrevPressed = mappedInput.wasReleased(MappedInputManager::Button::Up);
    rowNextPressed = mappedInput.wasReleased(MappedInputManager::Button::Down);
    wordPrevPressed = mappedInput.wasReleased(MappedInputManager::Button::Left);
    wordNextPressed = mappedInput.wasReleased(MappedInputManager::Button::Right);
  }

  const int rowCount = static_cast<int>(rows.size());

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

  if (rowPrevPressed) {
    int targetRow = (currentRow > 0) ? currentRow - 1 : rowCount - 1;
    currentWordInRow = findClosestWord(targetRow);
    currentRow = targetRow;
    changed = true;
  }

  if (rowNextPressed) {
    int targetRow = (currentRow < rowCount - 1) ? currentRow + 1 : 0;
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

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    int wordIdx = rows[currentRow].wordIndices[currentWordInRow];
    const std::string& rawWord = words[wordIdx].lookupText;
    std::string cleaned = Dictionary::cleanWord(rawWord);

    if (cleaned.empty()) {
      GUI.drawPopup(renderer, tr(STR_DICT_NO_WORD));
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      requestUpdate();
      return;
    }

    // Show "Looking up..." popup via render task, then do blocking lookup
    isLookingUp = true;
    lookupProgress = 0;
    requestUpdateAndWait();

    bool cancelled = false;
    std::string definition = Dictionary::lookup(
        cleaned,
        [this](int percent) {
          lookupProgress = percent;
          requestUpdateAndWait();
        },
        [this, &cancelled]() -> bool {
          mappedInput.update();
          if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
            cancelled = true;
            return true;
          }
          return false;
        });

    isLookingUp = false;

    if (cancelled) {
      requestUpdate();
      return;
    }

    LookupHistory::addWord(cachePath, cleaned);

    if (!definition.empty()) {
      startActivityForResult(
          std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, cleaned, definition, fontId, true),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              setResult(ActivityResult{});
              finish();
            } else {
              requestUpdate();
            }
          });
      return;
    }

    // Try stem variants
    auto stems = Dictionary::getStemVariants(cleaned);
    for (const auto& stem : stems) {
      std::string stemDef = Dictionary::lookup(stem);
      if (!stemDef.empty()) {
        startActivityForResult(
            std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, stem, stemDef, fontId, true),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                setResult(ActivityResult{});
                finish();
              } else {
                requestUpdate();
              }
            });
        return;
      }
    }

    // Offer synonym search before falling back to fuzzy suggestions
    if (Dictionary::hasSyn()) {
      synSearchWord = cleaned;
      isAskingSynonymSearch = true;
      requestUpdate();
      return;
    }

    handleNotFound(cleaned);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult r;
    r.isCancelled = true;
    setResult(std::move(r));
    finish();
    return;
  }

  if (changed) {
    requestUpdate();
  }
}

void DictionaryWordSelectActivity::render(RenderLock&&) {
  renderer.clearScreen();

  // "Search synonyms?" prompt
  if (isAskingSynonymSearch) {
    const int pageWidth = renderer.getScreenWidth();
    const auto& metrics = UITheme::getInstance().getMetrics();
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   tr(STR_DICT_SEARCH_SYNONYMS));
    const int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing +
                  renderer.getLineHeight(UI_10_FONT_ID);
    renderer.drawCenteredText(UI_10_FONT_ID, y, synSearchWord.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  // If looking up, show popup
  if (isLookingUp) {
    Rect popupLayout = GUI.drawPopup(renderer, tr(STR_LOOKING_UP));
    if (lookupProgress > 0) {
      GUI.fillPopupProgress(renderer, popupLayout, lookupProgress);
    }
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  // Render the page content
  page->render(renderer, fontId, marginLeft, marginTop);

  if (!words.empty() && currentRow < static_cast<int>(rows.size())) {
    int wordIdx = rows[currentRow].wordIndices[currentWordInRow];
    const auto& w = words[wordIdx];

    const int lineHeight = renderer.getLineHeight(fontId);
    renderer.fillRect(w.screenX - 1, w.screenY - 1, w.width + 2, lineHeight + 2, true);
    renderer.drawText(fontId, w.screenX, w.screenY, w.text.c_str(), false);

    // Highlight the other half of a hyphenated word
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
