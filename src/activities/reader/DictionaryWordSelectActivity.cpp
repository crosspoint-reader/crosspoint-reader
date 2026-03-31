#include "DictionaryWordSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>

#include "CrossPointSettings.h"
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
  std::string textPool;
  textPool.reserve(512);
  extractWords(words, rows, textPool);
  mergeHyphenatedWords(words, rows, textPool);
  navigator.load(std::move(words), std::move(rows), std::move(textPool));
  requestUpdate();
}

void DictionaryWordSelectActivity::onExit() {
  controller.onExit();
  Activity::onExit();
}

void DictionaryWordSelectActivity::extractWords(std::vector<WordSelectNavigator::WordInfo>& words,
                                                std::vector<WordSelectNavigator::Row>& rows, std::string& textPool) {
  words.clear();
  words.reserve(64);
  rows.clear();
  rows.reserve(16);

  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block) continue;

    const auto& wordList = block->getWords();
    const auto& xPosList = block->getWordXpos();
    const auto& styleList = block->getWordStyles();

    auto wordIt = wordList.begin();
    auto xIt = xPosList.begin();
    auto styleIt = styleList.begin();

    while (wordIt != wordList.end() && xIt != xPosList.end()) {
      int16_t screenX = line->xPos + static_cast<int16_t>(*xIt) + marginLeft;
      int16_t screenY = line->yPos + marginTop;
      const std::string& wordText = *wordIt;
      const EpdFontFamily::Style wordStyle = (styleIt != styleList.end()) ? *styleIt : EpdFontFamily::REGULAR;

      // Skip tokens with no alphanumeric characters (bullets, punctuation, etc.)
      if (!std::any_of(wordText.begin(), wordText.end(), [](unsigned char c) { return std::isalnum(c); })) {
        ++wordIt;
        ++xIt;
        if (styleIt != styleList.end()) ++styleIt;
        continue;
      }

      // Split on en-dash (U+2013: E2 80 93) and em-dash (U+2014: E2 80 94)
      std::vector<size_t> splitStarts;
      splitStarts.reserve(4);
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
        int16_t wordWidth = renderer.getTextWidth(SETTINGS.getReaderFontId(), wordText.c_str(), wordStyle);
        {
          uint16_t off = WordSelectNavigator::poolAppend(textPool, wordText.c_str(), wordText.size());
          WordSelectNavigator::WordInfo wi;
          wi.textOffset = off;
          wi.textLen = static_cast<uint16_t>(wordText.size());
          wi.lookupOffset = off;
          wi.lookupLen = wi.textLen;
          wi.screenX = screenX;
          wi.screenY = screenY;
          wi.width = wordWidth;
          wi.style = wordStyle;
          words.push_back(wi);
        }
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
          int16_t offsetX =
              prefix.empty() ? 0 : renderer.getTextWidth(SETTINGS.getReaderFontId(), prefix.c_str(), wordStyle);
          int16_t partWidth = renderer.getTextWidth(SETTINGS.getReaderFontId(), part.c_str(), wordStyle);
          {
            uint16_t off = WordSelectNavigator::poolAppend(textPool, part.c_str(), part.size());
            WordSelectNavigator::WordInfo wi;
            wi.textOffset = off;
            wi.textLen = static_cast<uint16_t>(part.size());
            wi.lookupOffset = off;
            wi.lookupLen = wi.textLen;
            wi.screenX = static_cast<int16_t>(screenX + offsetX);
            wi.screenY = screenY;
            wi.width = partWidth;
            wi.style = wordStyle;
            words.push_back(wi);
          }
        }
      }

      ++wordIt;
      ++xIt;
      if (styleIt != styleList.end()) ++styleIt;
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
                                                        std::vector<WordSelectNavigator::Row>& rows,
                                                        std::string& textPool) {
  for (size_t r = 0; r + 1 < rows.size(); r++) {
    if (rows[r].wordIndices.empty() || rows[r + 1].wordIndices.empty()) continue;

    int lastWordIdx = rows[r].wordIndices.back();
    const char* lastWord = textPool.data() + words[lastWordIdx].textOffset;
    uint16_t lastLen = words[lastWordIdx].textLen;
    if (lastLen == 0) continue;

    bool endsWithHyphen = false;
    if (lastWord[lastLen - 1] == '-') {
      endsWithHyphen = true;
    } else if (lastLen >= 2 && static_cast<uint8_t>(lastWord[lastLen - 2]) == 0xC2 &&
               static_cast<uint8_t>(lastWord[lastLen - 1]) == 0xAD) {
      endsWithHyphen = true;
    }
    if (!endsWithHyphen) continue;

    int nextWordIdx = rows[r + 1].wordIndices.front();
    words[lastWordIdx].continuationIndex = nextWordIdx;
    words[nextWordIdx].continuationOf = lastWordIdx;

    std::string firstPart(lastWord, lastLen);
    if (firstPart.back() == '-') {
      firstPart.pop_back();
    } else if (firstPart.size() >= 2 && static_cast<uint8_t>(firstPart[firstPart.size() - 2]) == 0xC2 &&
               static_cast<uint8_t>(firstPart[firstPart.size() - 1]) == 0xAD) {
      firstPart.erase(firstPart.size() - 2);
    }
    const char* nextWord = textPool.data() + words[nextWordIdx].textOffset;
    std::string merged = firstPart + nextWord;
    uint16_t mergedOff = WordSelectNavigator::poolAppend(textPool, merged.c_str(), merged.size());
    words[lastWordIdx].lookupOffset = mergedOff;
    words[lastWordIdx].lookupLen = static_cast<uint16_t>(merged.size());
    words[nextWordIdx].lookupOffset = mergedOff;
    words[nextWordIdx].lookupLen = static_cast<uint16_t>(merged.size());
    words[nextWordIdx].continuationIndex = nextWordIdx;
  }

  // Cross-page hyphenation
  if (!nextPageFirstWord.empty() && !rows.empty()) {
    int lastWordIdx = rows.back().wordIndices.back();
    const char* lastWord = textPool.data() + words[lastWordIdx].textOffset;
    uint16_t lastLen = words[lastWordIdx].textLen;
    if (lastLen > 0) {
      bool endsWithHyphen = false;
      if (lastWord[lastLen - 1] == '-') {
        endsWithHyphen = true;
      } else if (lastLen >= 2 && static_cast<uint8_t>(lastWord[lastLen - 2]) == 0xC2 &&
                 static_cast<uint8_t>(lastWord[lastLen - 1]) == 0xAD) {
        endsWithHyphen = true;
      }
      if (endsWithHyphen) {
        std::string firstPart(lastWord, lastLen);
        if (firstPart.back() == '-') {
          firstPart.pop_back();
        } else if (firstPart.size() >= 2 && static_cast<uint8_t>(firstPart[firstPart.size() - 2]) == 0xC2 &&
                   static_cast<uint8_t>(firstPart[firstPart.size() - 1]) == 0xAD) {
          firstPart.erase(firstPart.size() - 2);
        }
        std::string merged = firstPart + nextPageFirstWord;
        uint16_t off = WordSelectNavigator::poolAppend(textPool, merged.c_str(), merged.size());
        words[lastWordIdx].lookupOffset = off;
        words[lastWordIdx].lookupLen = static_cast<uint16_t>(merged.size());
      }
    }
  }

  rows.erase(
      std::remove_if(rows.begin(), rows.end(), [](const WordSelectNavigator::Row& r) { return r.wordIndices.empty(); }),
      rows.end());
}

// Shared helper: run findSimilar for `word` and launch suggestions activity, or show "not found" popup.
void DictionaryWordSelectActivity::handleNotFound(const std::string& word) {
  auto similar = Dictionary::findSimilar(word, 6, cachePath.c_str());
  if (!similar.empty()) {
    startActivityForResult(
        std::make_unique<DictionarySuggestionsActivity>(renderer, mappedInput, std::move(similar)),
        [this](const ActivityResult& result) {
          if (result.isCancelled) {
            controller.setNotFound();
            return;
          }
          const auto& wr = std::get<WordResult>(result.data);
          std::string def = Dictionary::lookup(wr.word, {}, cachePath.c_str());
          if (!def.empty()) {
            int chainStart = LookupHistory::addWord(cachePath, wr.word, LookupHistory::Status::Suggestion);
            startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, wr.word, def,
                                                                                  true, cachePath, chainStart),
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
    phrase += navigator.getDisplay(*w);
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
                                   true, cachePath, chainStart),
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
    std::string cleaned = Dictionary::cleanWord(navigator.getLookup(*sel));

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
  page->render(renderer, SETTINGS.getReaderFontId(), marginLeft, marginTop);

  const int lineHeight = renderer.getLineHeight(SETTINGS.getReaderFontId());
  if (inMultiSelectMode) {
    const int cursorIdx = navigator.getCurrentFlatIndex();
    const int lo = std::min(anchorFlatIndex, cursorIdx);
    const int hi = std::max(anchorFlatIndex, cursorIdx);
    for (int i = lo; i <= hi; i++) {
      const auto* w = navigator.getWordAt(i);
      if (!w) continue;
      renderer.fillRect(w->screenX - 2, w->screenY - 2, w->width + 4, lineHeight + 4, true);
      renderer.drawText(SETTINGS.getReaderFontId(), w->screenX, w->screenY, navigator.getDisplay(*w), false, w->style);
    }
  } else {
    if (const auto* w = navigator.getSelected()) {
      renderer.fillRect(w->screenX - 2, w->screenY - 2, w->width + 4, lineHeight + 4, true);
      renderer.drawText(SETTINGS.getReaderFontId(), w->screenX, w->screenY, navigator.getDisplay(*w), false, w->style);

      // Highlight the other half of a hyphenated word
      if (const auto* other = navigator.getContinuation()) {
        renderer.fillRect(other->screenX - 2, other->screenY - 2, other->width + 4, lineHeight + 4, true);
        renderer.drawText(SETTINGS.getReaderFontId(), other->screenX, other->screenY, navigator.getDisplay(*other),
                          false, other->style);
      }
    }
  }

  const auto labels = mappedInput.mapLabels("", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
