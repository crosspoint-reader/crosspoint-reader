#include "EpubReaderFootnotesActivity.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void EpubReaderFootnotesActivity::onEnter() {
  selectedIndex = 0;
  render();
}

void EpubReaderFootnotesActivity::onExit() {
  // Nothing to clean up
}

void EpubReaderFootnotesActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const FootnoteEntry* entry = footnotes.getEntry(selectedIndex);
    if (entry) {
      Serial.printf("[%lu] [FNS] Selected footnote: %s -> %s\n", millis(), entry->number, entry->href);
      onSelectFootnote(entry->href);
    }
    return;
  }

  buttonNavigator.onNext([this] {
    if (selectedIndex < footnotes.getCount() - 1) {
      selectedIndex++;
      render();
    }
  });

  buttonNavigator.onPrevious([this] {
    if (selectedIndex > 0) {
      selectedIndex--;
      render();
    }
  });
}

void EpubReaderFootnotesActivity::render() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();

  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;

  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;

  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int contentY = hintGutterHeight;

  const int marginLeft = contentX + 20;
  const int startY = contentY + 50;
  constexpr int lineHeight = 40;

  renderer.drawText(UI_12_FONT_ID, marginLeft, contentY + 20, "Footnotes", EpdFontFamily::BOLD);

  if (footnotes.getCount() == 0) {
    renderer.drawCenteredText(SMALL_FONT_ID, startY + 20, "No footnotes on this page");
    const auto labels = mappedInput.mapLabels("« Back", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  for (int i = 0; i < footnotes.getCount(); i++) {
    const FootnoteEntry* entry = footnotes.getEntry(i);
    if (!entry) continue;

    const int y = startY + i * lineHeight;
    const bool isSelected = (i == selectedIndex);

    if (isSelected) {
      renderer.fillRect(contentX, y, contentWidth, lineHeight, true);
    }
    const std::string text = entry->number;

    renderer.drawText(UI_12_FONT_ID, marginLeft + 10, y, text.c_str(), !isSelected);
  }

  // 4. Footer / Hints
  const auto labels = mappedInput.mapLabels("« Back", "Select", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}