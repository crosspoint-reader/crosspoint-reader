#include "LookedUpWordsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
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
        const int chainStart =
            LookupHistory::addWord(cachePath, controller.getLookupWord(), toHistStatus(controller.getFoundStatus()));
        startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(
                                   renderer, mappedInput, controller.getFoundWord(), controller.getFoundDefinition(),
                                   readerFontId, true, cachePath, chainStart),
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
      case DictionaryLookupController::LookupEvent::LookupFailed:
        LookupHistory::addWord(cachePath, controller.getLookupWord(), LookupHistory::Status::NotFound);
        entries = LookupHistory::load(cachePath);
        controller.setNotFound();
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

  if (entries.empty()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      ActivityResult r;
      r.isCancelled = true;
      setResult(std::move(r));
      finish();
    }
    return;
  }

  if (deleteConfirmMode) {
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

  const bool prevItem = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextItem = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Right);

  if (prevItem && selectedIndex > 0) {
    selectedIndex--;
    requestUpdate();
  }
  if (nextItem && selectedIndex < static_cast<int>(entries.size()) - 1) {
    selectedIndex++;
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (mappedInput.getHeldTime() >= LONG_PRESS_MS) {
      deleteConfirmMode = true;
      requestUpdate();
      return;
    }
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

  // Reserve a hint line above the button hints bar.
  const int hintLineHeight = 20;
  const int hintY = pageHeight - metrics.buttonHintsHeight - hintLineHeight;
  const int contentHeight = hintY - contentTop - metrics.verticalSpacing;

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
    renderer.drawCenteredText(UI_10_FONT_ID, hintY + 2, tr(STR_LOOKUP_HISTORY_DELETE_HINT));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
