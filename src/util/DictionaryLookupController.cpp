#include "DictionaryLookupController.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../activities/Activity.h"
#include "../activities/reader/DictionarySuggestionsActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"

DictionaryLookupController::DictionaryLookupController(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       Activity& owner, std::string cachePath)
    : renderer(renderer), mappedInput(mappedInput), owner(owner), cachePath(std::move(cachePath)) {}

void DictionaryLookupController::startLookup(const std::string& word) {
  lookupWord = word;
  foundWord.clear();
  foundDefinition.clear();
  lookupProgress = 0;
  lookupDone = false;
  lookupCancelled = false;
  lookupCancelRequested = false;
  state = LookupState::LookingUp;
  owner.requestUpdateAndWait();
  xTaskCreate(taskEntry, "DictLookup", 4096, this, 1, &taskHandle);
}

void DictionaryLookupController::startLookupAsSuggestion(const std::string& word) {
  nextIsSuggestion = true;
  startLookup(word);
}

void DictionaryLookupController::setNotFound() {
  state = LookupState::NotFound;
  owner.requestUpdate();
}

void DictionaryLookupController::onExit() {
  if (taskHandle != nullptr) {
    vTaskDelete(taskHandle);
    taskHandle = nullptr;
  }
}

DictionaryLookupController::LookupEvent DictionaryLookupController::handleInput() {
  if (state == LookupState::LookingUp) {
    if (lookupDone) {
      state = LookupState::Idle;
      taskHandle = nullptr;

      if (lookupCancelled) {
        nextIsSuggestion = false;
        return LookupEvent::Cancelled;
      }

      if (!foundDefinition.empty()) {
        foundWord = lookupWord;
        foundStatus = nextIsSuggestion ? FoundStatus::Suggestion : FoundStatus::Direct;
        nextIsSuggestion = false;
        return LookupEvent::FoundDefinition;
      }

      // Try stem variants
      auto stems = Dictionary::getStemVariants(lookupWord);
      for (const auto& stem : stems) {
        std::string stemDef = Dictionary::lookup(stem, {}, cachePath.c_str());
        if (!stemDef.empty()) {
          foundWord = stem;
          foundDefinition = stemDef;
          foundStatus = nextIsSuggestion ? FoundStatus::Suggestion : FoundStatus::Stem;
          nextIsSuggestion = false;
          return LookupEvent::FoundDefinition;
        }
      }

      // Try alt forms
      if (Dictionary::hasAltForms(cachePath.c_str())) {
        altFormWord = lookupWord;
        state = LookupState::AltFormPrompt;
        return LookupEvent::None;
      }

      handleLookupFailed();
      return LookupEvent::None;
    }

    // Task still running — check for cancel
    if (!lookupCancelRequested && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      lookupCancelRequested = true;
      owner.requestUpdate();
    }
    return LookupEvent::None;
  }

  if (state == LookupState::AltFormPrompt) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state = LookupState::Idle;
      std::string canonical = Dictionary::resolveAltForm(altFormWord, cachePath.c_str());
      if (!canonical.empty()) {
        std::string def = Dictionary::lookup(canonical, {}, cachePath.c_str());
        if (!def.empty()) {
          foundWord = canonical;
          foundDefinition = def;
          foundStatus = nextIsSuggestion ? FoundStatus::Suggestion : FoundStatus::AltForm;
          nextIsSuggestion = false;
          return LookupEvent::FoundDefinition;
        }
      }
      handleLookupFailed();
      return LookupEvent::None;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = LookupState::Idle;
      nextIsSuggestion = false;
      return LookupEvent::Cancelled;
    }
    return LookupEvent::None;
  }

  if (state == LookupState::NotFound) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state = LookupState::Idle;
      return LookupEvent::NotFoundDismissedDone;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = LookupState::Idle;
      return LookupEvent::NotFoundDismissedBack;
    }
    return LookupEvent::None;
  }

  return LookupEvent::None;
}

bool DictionaryLookupController::render() {
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (state == LookupState::LookingUp) {
    Rect popupLayout = GUI.drawPopup(renderer, tr(STR_DICT_LOOKING_UP));
    if (lookupProgress > 0) {
      GUI.fillPopupProgress(renderer, popupLayout, lookupProgress);
    }
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return true;
  }

  if (state == LookupState::AltFormPrompt) {
    const int pageWidth = renderer.getScreenWidth();
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   tr(STR_DICT_SEARCH_ALT_FORMS));
    const int y =
        metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + renderer.getLineHeight(UI_10_FONT_ID);
    renderer.drawCenteredText(UI_10_FONT_ID, y, altFormWord.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return true;
  }

  if (state == LookupState::NotFound) {
    GUI.drawPopup(renderer, tr(STR_DICT_NOT_FOUND));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return true;
  }

  return false;
}

void DictionaryLookupController::handleLookupFailed() {
  auto similar = Dictionary::findSimilar(lookupWord, 6, cachePath.c_str());
  if (!similar.empty()) {
    owner.startActivityForResult(
        std::make_unique<DictionarySuggestionsActivity>(renderer, mappedInput, std::move(similar)),
        [this](const ActivityResult& result) {
          if (result.isCancelled) {
            setNotFound();
            return;
          }
          const auto& wr = std::get<WordResult>(result.data);
          startLookupAsSuggestion(wr.word);
        });
    return;
  }
  if (!cachePath.empty()) {
    LookupHistory::addWord(cachePath, lookupWord, LookupHistory::Status::NotFound);
  }
  nextIsSuggestion = false;
  setNotFound();
}

void DictionaryLookupController::taskEntry(void* param) {
  DictionaryLookupController* self = static_cast<DictionaryLookupController*>(param);
  self->runLookup();
  self->taskHandle = nullptr;
  vTaskDelete(nullptr);
}

void DictionaryLookupController::progressCallback(void* ctx, int percent) {
  auto* self = static_cast<DictionaryLookupController*>(ctx);
  self->lookupProgress = percent;
  self->owner.requestUpdate(true);
}

bool DictionaryLookupController::cancelCallback(void* ctx) {
  return static_cast<DictionaryLookupController*>(ctx)->lookupCancelRequested;
}

void DictionaryLookupController::runLookup() {
  DictLookupCallbacks cbs;
  cbs.ctx = this;
  cbs.onProgress = &DictionaryLookupController::progressCallback;
  cbs.shouldCancel = &DictionaryLookupController::cancelCallback;
  foundDefinition = Dictionary::lookup(lookupWord, cbs, cachePath.c_str());
  lookupCancelled = lookupCancelRequested;
  lookupDone = true;
  owner.requestUpdate(true);
}
