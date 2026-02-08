#include "EpubReaderFootnotesActivity.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>

#include "MappedInputManager.h"
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

  bool needsRedraw = false;

  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    if (selectedIndex > 0) {
      selectedIndex--;
      needsRedraw = true;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    if (selectedIndex < footnotes.getCount() - 1) {
      selectedIndex++;
      needsRedraw = true;
    }
  }

  if (needsRedraw) {
    render();
  }
}

void EpubReaderFootnotesActivity::render() {
  renderer.clearScreen();

  constexpr int startY = 50;
  constexpr int lineHeight = 40;
  constexpr int marginLeft = 20;

  // Title
  renderer.drawText(UI_12_FONT_ID, marginLeft, 20, "Footnotes", EpdFontFamily::BOLD);

  if (footnotes.getCount() == 0) {
    renderer.drawText(SMALL_FONT_ID, marginLeft, startY + 20, "No footnotes on this page");
    renderer.displayBuffer();
    return;
  }

  // Display footnotes
  for (int i = 0; i < footnotes.getCount(); i++) {
    const FootnoteEntry* entry = footnotes.getEntry(i);
    if (!entry) continue;

    const int y = startY + i * lineHeight;

    // Draw selection indicator (arrow)
    if (i == selectedIndex) {
      renderer.drawText(UI_12_FONT_ID, marginLeft - 10, y, ">", EpdFontFamily::BOLD);
      renderer.drawText(UI_12_FONT_ID, marginLeft + 10, y, entry->number, EpdFontFamily::BOLD);
    } else {
      renderer.drawText(UI_12_FONT_ID, marginLeft + 10, y, entry->number);
    }
  }

  // Instructions at bottom
  renderer.drawText(SMALL_FONT_ID, marginLeft, renderer.getScreenHeight() - 40,
                    "UP/DOWN: Select  CONFIRM: Go to footnote  BACK: Return");

  renderer.displayBuffer();
}
