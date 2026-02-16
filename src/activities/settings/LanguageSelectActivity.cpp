#include "LanguageSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "fontIds.h"

void LanguageSelectActivity::onEnter() {
  Activity::onEnter();

  totalItems = getLanguageCount();

  // Set current selection based on current language
  selectedIndex = static_cast<int>(I18N.getLanguage());

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

  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex + totalItems - 1) % totalItems;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex + 1) % totalItems;
    requestUpdate();
  }
}

void LanguageSelectActivity::handleSelection() {
  {
    RenderLock lock(*this);
    I18N.setLanguage(static_cast<Language>(selectedIndex));
  }

  // Return to previous page
  onBack();
}

void LanguageSelectActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  constexpr int rowHeight = 30;

  // Title
  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_LANGUAGE), true, EpdFontFamily::BOLD);

  // Current language marker
  const int currentLang = static_cast<int>(I18N.getLanguage());

  // Draw options
  for (int i = 0; i < totalItems; i++) {
    const int itemY = 60 + i * rowHeight;
    const bool isSelected = (i == selectedIndex);
    const bool isCurrent = (i == currentLang);

    // Draw selection highlight
    if (isSelected) {
      renderer.fillRect(0, itemY - 2, pageWidth - 1, rowHeight);
    }

    // Draw language name - get it from i18n system
    const char* langName = I18N.getLanguageName(static_cast<Language>(i));
    renderer.drawText(UI_10_FONT_ID, 20, itemY, langName, !isSelected);

    // Draw current selection marker
    if (isCurrent) {
      const char* marker = tr(STR_ON_MARKER);
      const auto width = renderer.getTextWidth(UI_10_FONT_ID, marker);
      renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - width, itemY, marker, !isSelected);
    }
  }

  // Button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
