#include "DictionaryWordSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>

#include "DictionaryDefinitionActivity.h"
#include "DictionarySuggestionsActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/LookupHistory.h"

static LookupHistory::Status toHistStatus(DictionaryLookupController::FoundStatus fs) {
  switch (fs) {
    case DictionaryLookupController::FoundStatus::Direct:
      return LookupHistory::Status::Direct;
    case DictionaryLookupController::FoundStatus::Stem:
      return LookupHistory::Status::Stem;
    case DictionaryLookupController::FoundStatus::AltForm:
      return LookupHistory::Status::AltForm;
    case DictionaryLookupController::FoundStatus::Suggestion:
      return LookupHistory::Status::Suggestion;
    default:
      return LookupHistory::Status::NotFound;
  }
}

void DictionaryWordSelectActivity::onEnter() {
  Activity::onEnter();
  std::vector<WordSelectNavigator::WordInfo> words;
  std::vector<WordSelectNavigator::Row> rows;
  extractWords(words, rows);
  mergeHyphenatedWords(words, rows);
  navigator.load(std::move(words), std::move(rows));
  requestUpdate();
}

void DictionaryWordSelectActivity::onExit() {
  controller.onExit();
  Activity::onExit();
}

void DictionaryWordSelectActivity::extractWords(std::vector<WordSelectNavigator::WordInfo>& words,
                                                std::vector<WordSelectNavigator::Row>& rows) {
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

void DictionaryWordSelectActivity::mergeHyphenatedWords(std::vector<WordSelectNavigator::WordInfo>& words,
                                                        std::vector<WordSelectNavigator::Row>& rows) {
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

  rows.erase(
      std::remove_if(rows.begin(), rows.end(), [](const WordSelectNavigator::Row& r) { return r.wordIndices.empty(); }),
      rows.end());
}

// Shared helper: run findSimilar for `word` and launch suggestions activity, or show "not found" popup.
void DictionaryWordSelectActivity::handleNotFound(const std::string& word) {
  auto similar = Dictionary::findSimilar(word, 6);
  if (!similar.empty()) {
    startActivityForResult(
        std::make_unique<DictionarySuggestionsActivity>(renderer, mappedInput, std::move(similar), fontId),
        [this](const ActivityResult& result) {
          if (result.isCancelled) {
            controller.setNotFound();
            return;
          }
          const auto& wr = std::get<WordResult>(result.data);
          std::string def = Dictionary::lookup(wr.word);
          if (!def.empty()) {
            int chainStart = LookupHistory::addWord(cachePath, wr.word, LookupHistory::Status::Suggestion);
            startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, wr.word, def,
                                                                                  fontId, true, cachePath, chainStart),
                                   [this](const ActivityResult& r) {
                                     if (!r.isCancelled) {
                                       setResult(ActivityResult{});
                                       finish();
                                     } else {
                                       requestUpdate();
                                     }
                                   });
          } else {
            controller.setNotFound();
          }
        });
    return;
  }
  LookupHistory::addWord(cachePath, word, LookupHistory::Status::NotFound);
  controller.setNotFound();
}

std::string DictionaryWordSelectActivity::buildPhraseFromRange(int fromIdx, int toIdx) const {
  const int lo = std::min(fromIdx, toIdx);
  const int hi = std::max(fromIdx, toIdx);
  std::string phrase;
  for (int i = lo; i <= hi; i++) {
    const auto* w = navigator.getWordAt(i);
    if (!w) continue;
    if (!phrase.empty()) phrase += ' ';
    phrase += w->text;
  }
  return Dictionary::cleanWord(phrase);
}

void DictionaryWordSelectActivity::loop() {
  if (controller.isActive()) {
    switch (controller.handleInput()) {
      case DictionaryLookupController::LookupEvent::FoundDefinition: {
        int chainStart =
            LookupHistory::addWord(cachePath, controller.getLookupWord(), toHistStatus(controller.getFoundStatus()));
        startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(
                                   renderer, mappedInput, controller.getFoundWord(), controller.getFoundDefinition(),
                                   fontId, true, cachePath, chainStart),
                               [this](const ActivityResult& result) {
                                 if (!result.isCancelled) {
                                   setResult(ActivityResult{});
                                   finish();
                                 } else {
                                   requestUpdate();
                                 }
                               });
        break;
      }
      case DictionaryLookupController::LookupEvent::LookupFailed:
        handleNotFound(controller.getLookupWord());
        break;
      case DictionaryLookupController::LookupEvent::NotFoundDismissedBack:
        requestUpdate();
        break;
      case DictionaryLookupController::LookupEvent::NotFoundDismissedDone:
        setResult(ActivityResult{});
        finish();
        break;
      case DictionaryLookupController::LookupEvent::Cancelled:
        requestUpdate();
        break;
      default:
        break;
    }
    return;
  }

  if (navigator.isEmpty()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      ActivityResult r;
      r.isCancelled = true;
      setResult(std::move(r));
      finish();
    }
    return;
  }

  if (navigator.handleNavigation(mappedInput, renderer)) {
    requestUpdate();
  }

  if (inMultiSelectMode) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      const int cursorIdx = navigator.getCurrentFlatIndex();
      std::string phrase = buildPhraseFromRange(anchorFlatIndex, cursorIdx);
      if (phrase.empty()) {
        GUI.drawPopup(renderer, tr(STR_DICT_NO_WORD));
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        requestUpdate();
        return;
      }
      inMultiSelectMode = false;
      controller.startLookup(phrase);
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      inMultiSelectMode = false;
      requestUpdate();
      return;
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (mappedInput.getHeldTime() >= LONG_PRESS_MS) {
      const int flatIdx = navigator.getCurrentFlatIndex();
      if (flatIdx >= 0) {
        inMultiSelectMode = true;
        anchorFlatIndex = flatIdx;
        requestUpdate();
      }
      return;
    }

    const auto* sel = navigator.getSelected();
    if (!sel) return;
    std::string cleaned = Dictionary::cleanWord(sel->lookupText);

    if (cleaned.empty()) {
      GUI.drawPopup(renderer, tr(STR_DICT_NO_WORD));
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      requestUpdate();
      return;
    }

    controller.startLookup(cleaned);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult r;
    r.isCancelled = true;
    setResult(std::move(r));
    finish();
    return;
  }
}

void DictionaryWordSelectActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (controller.render()) return;

  // Render the page content
  page->render(renderer, fontId, marginLeft, marginTop);

  const int lineHeight = renderer.getLineHeight(fontId);
  if (inMultiSelectMode) {
    const int cursorIdx = navigator.getCurrentFlatIndex();
    const int lo = std::min(anchorFlatIndex, cursorIdx);
    const int hi = std::max(anchorFlatIndex, cursorIdx);
    for (int i = lo; i <= hi; i++) {
      const auto* w = navigator.getWordAt(i);
      if (!w) continue;
      renderer.fillRect(w->screenX - 1, w->screenY - 1, w->width + 2, lineHeight + 2, true);
      renderer.drawText(fontId, w->screenX, w->screenY, w->text.c_str(), false);
    }
  } else {
    if (const auto* w = navigator.getSelected()) {
      renderer.fillRect(w->screenX - 1, w->screenY - 1, w->width + 2, lineHeight + 2, true);
      renderer.drawText(fontId, w->screenX, w->screenY, w->text.c_str(), false);

      // Highlight the other half of a hyphenated word
      if (const auto* other = navigator.getContinuation()) {
        renderer.fillRect(other->screenX - 1, other->screenY - 1, other->width + 2, lineHeight + 2, true);
        renderer.drawText(fontId, other->screenX, other->screenY, other->text.c_str(), false);
      }
    }
  }

  const auto labels = mappedInput.mapLabels("", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
