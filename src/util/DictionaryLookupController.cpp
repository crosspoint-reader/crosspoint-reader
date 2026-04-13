#include "DictionaryLookupController.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../activities/Activity.h"
#include "../activities/reader/DictionarySuggestionsActivity.h"
#include "DictLookupTask.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"

DictionaryLookupController::DictionaryLookupController(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       Activity& owner, std::string cachePath)
    : renderer(renderer), mappedInput(mappedInput), owner(owner), cachePath(std::move(cachePath)) {}

DictionaryLookupController::~DictionaryLookupController() = default;

void DictionaryLookupController::startLookup(const std::string& word, bool recordHistory) {
  lookupWord = word;
  foundWord.clear();
  foundLocation = DictLocation{};
  lookupProgress = 0;
  lookupDone = false;
  lookupCancelled = false;
  lookupCancelRequested = false;
  recordHistory_ = recordHistory;
  state = LookupState::LookingUp;
  // Skip requestUpdateAndWait() - lookup is fast (~30ms), no need to show "looking up..."
  // overlay. This avoids triggering an e-ink refresh that would block the activity transition.
  task = std::make_unique<DictLookupTask>(*this);
  task->start("DictLookup", 4096, 1);
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
  if (task) {
    task->stop();
    task->wait();
    task.reset();
  }
}

DictionaryLookupController::LookupEvent DictionaryLookupController::handleInput() {
  if (state == LookupState::LookingUp) {
    if (lookupDone) {
      state = LookupState::Idle;
      task.reset();

      if (lookupCancelled) {
        nextIsSuggestion = false;
        return LookupEvent::Cancelled;
      }

      if (foundLocation.found) {
        foundWord = lookupWord;
        foundStatus = nextIsSuggestion ? FoundStatus::Suggestion : FoundStatus::Direct;
        nextIsSuggestion = false;
        // Defer history recording - will be done by recordPendingHistory() after definition is displayed
        pendingHistoryWord = recordHistory_ ? lookupWord : "";
        return LookupEvent::FoundDefinition;
      }

      // Try stem variants (locate only — no definition loaded into RAM)
      auto stems = Dictionary::getStemVariants(lookupWord);
      for (const auto& stem : stems) {
        auto loc = Dictionary::locate(stem, {}, cachePath.c_str());
        if (loc.found) {
          foundWord = stem;
          foundLocation = std::move(loc);
          foundStatus = nextIsSuggestion ? FoundStatus::Suggestion : FoundStatus::Stem;
          nextIsSuggestion = false;
          if (recordHistory_ && !cachePath.empty()) {
            LookupHistory::addWord(cachePath, lookupWord, toHistStatus(foundStatus));
          }
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
        auto loc = Dictionary::locate(canonical, {}, cachePath.c_str());
        if (loc.found) {
          foundWord = canonical;
          foundLocation = std::move(loc);
          foundStatus = nextIsSuggestion ? FoundStatus::Suggestion : FoundStatus::AltForm;
          nextIsSuggestion = false;
          if (recordHistory_ && !cachePath.empty()) {
            LookupHistory::addWord(cachePath, lookupWord, toHistStatus(foundStatus));
          }
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
    return false;  // Skip popup, let lookup proceed silently
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

bool DictionaryLookupController::handleMultiSelect(WordSelectNavigator& navigator) {
  std::string msPhrase;
  const auto msAction = navigator.handleMultiSelectInput(mappedInput, msPhrase);
  if (msAction == WordSelectNavigator::MultiSelectAction::None) return false;
  switch (msAction) {
    case WordSelectNavigator::MultiSelectAction::PhraseReady:
      lookupOrPopup(msPhrase);
      return true;
    case WordSelectNavigator::MultiSelectAction::ExitedMultiSelect:
    case WordSelectNavigator::MultiSelectAction::EnteredMultiSelect:
      owner.requestUpdate();
      return true;
    default:
      return true;
  }
}

bool DictionaryLookupController::handleConfirmLookup(const WordSelectNavigator& navigator) {
  if (!mappedInput.wasReleased(MappedInputManager::Button::Confirm)) return false;
  const auto* sel = navigator.getSelected();
  if (!sel) return true;  // consumed input even if nothing selected
  lookupOrPopup(navigator.getLookup(*sel));
  return true;
}

void DictionaryLookupController::lookupOrPopup(const std::string& rawWord) {
  std::string cleaned = Dictionary::cleanWord(rawWord);
  if (cleaned.empty()) {
    showNoWordPopup();
  } else {
    startLookup(cleaned);
  }
}

void DictionaryLookupController::showNoWordPopup() {
  GUI.drawPopup(renderer, tr(STR_DICT_NO_WORD));
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  vTaskDelay(1000 / portTICK_PERIOD_MS);
  owner.requestUpdate();
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
  foundLocation = Dictionary::locate(lookupWord, cbs, cachePath.c_str());
  lookupCancelled = lookupCancelRequested;
  lookupDone = true;
  // Don't call requestUpdate(true) here - it triggers an unnecessary e-ink refresh
  // of the word select activity before transitioning to the definition activity.
  // The main loop polls lookupDone every ~10ms, so response time is still fast.
}

void DictionaryLookupController::recordPendingHistory() {
  if (!pendingHistoryWord.empty() && !cachePath.empty()) {
    LookupHistory::addWord(cachePath, pendingHistoryWord, toHistStatus(foundStatus));
    pendingHistoryWord.clear();
  }
}
