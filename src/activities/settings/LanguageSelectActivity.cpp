#include "LanguageSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "I18nKeys.h"
#include "LanguageDownloadActivity.h"
#include "LanguageManifestFetchActivity.h"
#include "LanguageRegistry.h"
#include "MappedInputManager.h"
#include "fontIds.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

int LanguageSelectActivity::totalListItems() const {
  // One extra "More languages..." entry when no manifest is cached yet.
  return static_cast<int>(_languages.size()) + (_hasManifest ? 0 : 1);
}

void LanguageSelectActivity::refreshList() {
  _languages = LanguageRegistry::buildList();
  _hasManifest = LanguageRegistry::hasManifest();

  const int total = totalListItems();
  if (total <= 0) {
    selectedIndex = 0;
  } else if (selectedIndex < 0) {
    selectedIndex = 0;
  } else if (selectedIndex >= total) {
    selectedIndex = total - 1;
  }
}

// ---------------------------------------------------------------------------
// Activity lifecycle
// ---------------------------------------------------------------------------

void LanguageSelectActivity::onEnter() {
  Activity::onEnter();
  refreshList();

  // Find the display position of the active language.
  const char* activeCode = I18N.getActiveCode();
  selectedIndex = 0;
  for (int i = 0; i < static_cast<int>(_languages.size()); i++) {
    if (strcmp(_languages[i].code, activeCode) == 0) {
      selectedIndex = i;
      break;
    }
  }

  requestUpdate();
}

void LanguageSelectActivity::onExit() { Activity::onExit(); }

void LanguageSelectActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int total = totalListItems();

  buttonNavigator.onNextRelease([this, total] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, total);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, total] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, total);
    requestUpdate();
  });
}

void LanguageSelectActivity::handleSelection() {
  const int total = totalListItems();
  if (total <= 0 || selectedIndex < 0 || selectedIndex >= total) {
    selectedIndex = 0;
    requestUpdate();
    return;
  }

  // "More languages..." entry (last item, only when no manifest cached).
  if (!_hasManifest && selectedIndex == static_cast<int>(_languages.size())) {
    startActivityForResult(std::make_unique<LanguageManifestFetchActivity>(renderer, mappedInput),
                           [this](const ActivityResult& res) {
                             if (!res.isCancelled) {
                               refreshList();
                               requestUpdate();
                             }
                           });
    return;
  }

  const LanguageEntry& lang = _languages[selectedIndex];

  if (lang.isCore) {
    // Core language: activate via enum index.
    // Core langs occupy the first LANGUAGE_META_COUNT entries of ALL_LANGUAGES
    // in the same order as the Language enum.
    for (uint8_t i = 0; i < LANGUAGE_META_COUNT; i++) {
      if (strcmp(ALL_LANGUAGES[i].code, lang.code) == 0) {
        RenderLock lock(*this);
        I18N.setLanguage(static_cast<Language>(i));
        onBack();
        return;
      }
    }
    return;
  }

  if (lang.isInstalled) {
    // Non-core language already on SD card: load it.
    RenderLock lock(*this);
    if (I18N.setExternalLanguage(lang.code)) onBack();
    return;
  }

  // Non-core language available for download.
  startActivityForResult(std::make_unique<LanguageDownloadActivity>(renderer, mappedInput, lang.code, lang.name),
                         [this](const ActivityResult& res) {
                           if (!res.isCancelled) onBack();  // language activated — leave the list
                         });
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void LanguageSelectActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_LANGUAGE));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  const char* activeCode = I18N.getActiveCode();
  const int total = totalListItems();

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, total, selectedIndex,
      // Row title
      [this](int index) -> std::string {
        if (!_hasManifest && index == static_cast<int>(_languages.size())) return tr(STR_GET_MORE_LANGUAGES);
        return std::string(_languages[index].name);
      },
      nullptr, nullptr,
      // Row status
      [this, activeCode](int index) -> std::string {
        if (!_hasManifest && index == static_cast<int>(_languages.size())) return "";
        const LanguageEntry& lang = _languages[index];
        if (strcmp(lang.code, activeCode) == 0) return tr(STR_SELECTED);
        if (!lang.isInstalled) return tr(STR_DOWNLOAD);
        return "";
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
