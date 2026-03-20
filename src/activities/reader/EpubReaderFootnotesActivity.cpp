#include "EpubReaderFootnotesActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void EpubReaderFootnotesActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void EpubReaderFootnotesActivity::onExit() { ActivityWithSubactivity::onExit(); }

void EpubReaderFootnotesActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(footnotes.size())) {
      onSelectFootnote(footnotes[selectedIndex].href);
    }
    return;
  }

  buttonNavigator.onNext([this] {
    if (!footnotes.empty()) {
      selectedIndex = (selectedIndex + 1) % footnotes.size();
      requestUpdate();
    }
  });

  buttonNavigator.onPrevious([this] {
    if (!footnotes.empty()) {
      selectedIndex = (selectedIndex - 1 + footnotes.size()) % footnotes.size();
      requestUpdate();
    }
  });
}

void EpubReaderFootnotesActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_FOOTNOTES), true, EpdFontFamily::BOLD);

  if (footnotes.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, 90, tr(STR_NO_FOOTNOTES));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  constexpr int startY = 50;
  constexpr int lineHeight = 36;
  const int screenWidth = renderer.getScreenWidth();
  constexpr int marginLeft = 20;

  const int visibleCount = std::max(1, (renderer.getScreenHeight() - startY) / lineHeight);
  if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
  if (selectedIndex >= scrollOffset + visibleCount) scrollOffset = selectedIndex - visibleCount + 1;

  for (int i = scrollOffset; i < static_cast<int>(footnotes.size()) && i < scrollOffset + visibleCount; i++) {
    const int y = startY + (i - scrollOffset) * lineHeight;
    const bool isSelected = (i == selectedIndex);

    if (isSelected) {
      renderer.fillRect(0, y, screenWidth, lineHeight, true);
    }

    // Show footnote number and abbreviated href
    std::string label = footnotes[i].number;
    if (label.empty()) {
      label = tr(STR_LINK);
    }
    renderer.drawText(UI_10_FONT_ID, marginLeft, y + 4, label.c_str(), !isSelected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
