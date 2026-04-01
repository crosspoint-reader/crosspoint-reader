#include "LookedUpWordsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/LookupHistory.h"

const char* LookedUpWordsActivity::glyphFor(LookupHistory::Status s) {
  switch (s) {
    case LookupHistory::Status::Direct:
      return "\xe2\x88\x9a";  // √ U+221A
    case LookupHistory::Status::Stem:
      return "~";
    case LookupHistory::Status::AltForm:
      return "~";
    case LookupHistory::Status::Suggestion:
      return "?";
    case LookupHistory::Status::NotFound:
      return "\xc3\x97";  // × U+00D7
    default:
      return "?";
  }
}

void LookedUpWordsActivity::onEnter() {
  Activity::onEnter();
  entries = LookupHistory::load(cachePath);
  requestUpdate();
}

void LookedUpWordsActivity::onExit() {
  controller.onExit();
  Activity::onExit();
}

void LookedUpWordsActivity::loop() {
  if (controller.isActive()) {
    switch (controller.handleInput()) {
      case DictionaryLookupController::LookupEvent::FoundDefinition: {
        LookupHistory::addWord(cachePath, controller.getLookupWord(),
                               DictionaryLookupController::toHistStatus(controller.getFoundStatus()));
        startActivityForResult(
            std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, controller.getFoundWord(),
                                                           controller.getFoundDefinition(), true, cachePath),
            [this](const ActivityResult& result) {
              entries = LookupHistory::load(cachePath);
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
        entries = LookupHistory::load(cachePath);
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

  if (entries.empty()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      ActivityResult r;
      r.isCancelled = true;
      setResult(std::move(r));
      finish();
    }
    return;
  }

  // Long press Confirm: enter delete-confirm mode (fire at threshold).
  if (!deleteConfirmMode && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= Dictionary::LONG_PRESS_MS) {
    deleteConfirmMode = true;
    confirmReleaseConsumed = true;
    requestUpdate();
    return;
  }

  if (deleteConfirmMode) {
    // Consume the Confirm release that follows the threshold-fire.
    if (confirmReleaseConsumed && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      confirmReleaseConsumed = false;
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      LookupHistory::removeAt(cachePath, fileIndexOf(selectedIndex));
      entries = LookupHistory::load(cachePath);
      deleteConfirmMode = false;
      if (selectedIndex >= static_cast<int>(entries.size())) {
        selectedIndex = std::max(0, static_cast<int>(entries.size()) - 1);
      }
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      deleteConfirmMode = false;
      requestUpdate();
      return;
    }
    return;
  }

  const int totalItems = static_cast<int>(entries.size());
  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);

  buttonNavigator.onNextRelease([this, totalItems] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, totalItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, totalItems] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, totalItems);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this, totalItems, pageItems] {
    selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, totalItems, pageItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, totalItems, pageItems] {
    selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, totalItems, pageItems);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    controller.startLookup(entries[selectedIndex].word);
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

void LookedUpWordsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (controller.render()) return;

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_LOOKUP_HISTORY));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  if (entries.empty()) {
    const int midY = contentTop + (pageHeight - contentTop - metrics.buttonHintsHeight) / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, midY, tr(STR_LOOKUP_HISTORY_EMPTY));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  const int contentHeight = pageHeight - metrics.buttonHintsHeight - contentTop - metrics.verticalSpacing;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(entries.size()), selectedIndex,
      [this](int i) { return std::string(glyphFor(entries[i].status)) + " " + entries[i].word; }, nullptr, nullptr,
      nullptr, false);

  if (deleteConfirmMode) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s: %s?", tr(STR_DELETE), entries[selectedIndex].word.c_str());
    GUI.drawPopup(renderer, buf);
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_DELETE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
