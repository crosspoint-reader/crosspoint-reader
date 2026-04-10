#include "DictionaryWordSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Utf8.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"

void DictionaryWordSelectActivity::onEnter() {
  Activity::onEnter();
  std::vector<WordSelectNavigator::WordInfo> words;
  std::vector<WordSelectNavigator::Row> rows;
  std::string textPool;
  textPool.reserve(512);
  extractWords(words, rows, textPool);
  mergeHyphenatedWords(words, rows, textPool);
  // consumeInitialConfirm=true: the long-press that opened word selection may still
  // be held, so ignore it until the user releases and presses again.
  navigator.load(std::move(words), std::move(rows), std::move(textPool), true);
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
          wi.fontId = SETTINGS.getReaderFontId();
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
            wi.fontId = SETTINGS.getReaderFontId();
            words.push_back(wi);
          }
        }
      }

      ++wordIt;
      ++xIt;
      if (styleIt != styleList.end()) ++styleIt;
    }
  }

  WordSelectNavigator::organizeIntoRows(words, rows);
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

    if (!utf8EndsWithHyphen(lastWord, lastLen)) continue;

    int nextWordIdx = rows[r + 1].wordIndices.front();
    words[lastWordIdx].continuationIndex = nextWordIdx;
    words[nextWordIdx].continuationOf = lastWordIdx;

    std::string firstPart(lastWord, lastLen);
    utf8RemoveTrailingHyphen(firstPart);
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
    if (lastLen > 0 && utf8EndsWithHyphen(lastWord, lastLen)) {
      std::string firstPart(lastWord, lastLen);
      utf8RemoveTrailingHyphen(firstPart);
      std::string merged = firstPart + nextPageFirstWord;
      uint16_t off = WordSelectNavigator::poolAppend(textPool, merged.c_str(), merged.size());
      words[lastWordIdx].lookupOffset = off;
      words[lastWordIdx].lookupLen = static_cast<uint16_t>(merged.size());
    }
  }

  rows.erase(
      std::remove_if(rows.begin(), rows.end(), [](const WordSelectNavigator::Row& r) { return r.wordIndices.empty(); }),
      rows.end());
}

void DictionaryWordSelectActivity::loop() {
  if (controller.isActive()) {
    switch (controller.handleInput()) {
      case DictionaryLookupController::LookupEvent::FoundDefinition: {
        startActivityForResult(
            std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, controller.getFoundWord(),
                                                           controller.getFoundLocation(), true, cachePath),
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

  if (controller.handleMultiSelect(navigator)) return;

  if (navigator.isMultiSelecting()) return;

  if (controller.handleConfirmLookup(navigator)) return;

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
  navigator.renderHighlight(renderer, lineHeight);

  const auto labels = mappedInput.mapLabels("", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
