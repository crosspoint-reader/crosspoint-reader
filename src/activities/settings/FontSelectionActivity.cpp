#include "FontSelectionActivity.h"

#include <EpdFontLoader.h>
#include <HardwareSerial.h>

#include "../../CrossPointSettings.h"
#include "../../fontIds.h"
#include "../../managers/FontManager.h"

FontSelectionActivity::FontSelectionActivity(GfxRenderer& renderer, MappedInputManager& inputManager,
                                             std::function<void()> onClose)
    : Activity("Font Selection", renderer, inputManager), onClose(onClose) {}

FontSelectionActivity::~FontSelectionActivity() {}

void FontSelectionActivity::onEnter() {
  Serial.println("[FSA] onEnter start");
  Activity::onEnter();
  Serial.println("[FSA] Getting available families...");
  fontFamilies = FontManager::getInstance().getAvailableFamilies();
  Serial.printf("[FSA] Got %d families\n", fontFamilies.size());

  std::string current = SETTINGS.customFontFamily;
  Serial.printf("[FSA] Current setting: %s\n", current.c_str());

  for (size_t i = 0; i < fontFamilies.size(); i++) {
    if (fontFamilies[i] == current) {
      selectedIndex = i;
      Serial.printf("[FSA] Found current family at index %d\n", i);
      // Adjust scroll
      if (selectedIndex >= itemsPerPage) {
        scrollOffset = selectedIndex - itemsPerPage / 2;
        if (scrollOffset > (int)fontFamilies.size() - itemsPerPage) {
          scrollOffset = std::max(0, (int)fontFamilies.size() - itemsPerPage);
        }
      }
      break;
    }
  }
  Serial.println("[FSA] Calling render()");
  render();
  Serial.println("[FSA] onEnter end");
}

void FontSelectionActivity::loop() {
  bool update = false;

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onClose();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex > 0) ? (selectedIndex - 1) : ((int)fontFamilies.size() - 1);
    update = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex < (int)fontFamilies.size() - 1) ? (selectedIndex + 1) : 0;
    update = true;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    saveAndExit();
    return;
  }

  if (update) {
    render();
  }
}

void FontSelectionActivity::saveAndExit() {
  if (selectedIndex >= 0 && selectedIndex < (int)fontFamilies.size()) {
    strncpy(SETTINGS.customFontFamily, fontFamilies[selectedIndex].c_str(), sizeof(SETTINGS.customFontFamily) - 1);
    SETTINGS.customFontFamily[sizeof(SETTINGS.customFontFamily) - 1] = '\0';
    SETTINGS.fontFamily = CrossPointSettings::FONT_CUSTOM;
    SETTINGS.saveToFile();
  }
  onClose();
}

void FontSelectionActivity::render() const {
  renderer.clearScreen();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Select Font", true, EpdFontFamily::BOLD);

  int y = 50;

  if (fontFamilies.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, 120, "No fonts found in /fonts", false);
    renderer.drawCenteredText(UI_10_FONT_ID, 150, "Add .epdfont files to SD Card", false);
    renderer.displayBuffer();  // ensure update
    return;
  }

  for (int i = 0; i < (int)fontFamilies.size(); i++) {
    int idx = scrollOffset + i;

    // Draw selection box
    if (idx == selectedIndex) {
      Serial.printf("[FSA] Drawing selected: %s at %d\n", fontFamilies[idx].c_str(), y);
      renderer.fillRect(0, y - 2, 480, 30);
      renderer.drawText(UI_10_FONT_ID, 20, y, fontFamilies[idx].c_str(), false);  // false = white (on black box)
    } else {
      Serial.printf("[FSA] Drawing: %s at %d\n", fontFamilies[idx].c_str(), y);
      renderer.drawText(UI_10_FONT_ID, 20, y, fontFamilies[idx].c_str(), true);  // true = black (on white bg)
    }

    // Mark current active font
    if (fontFamilies[idx] == SETTINGS.customFontFamily) {
      renderer.drawText(UI_10_FONT_ID, 400, y, "*", idx != selectedIndex);
    }

    y += 30;
  }

  // Draw help text
  const auto labels = mappedInput.mapLabels("« Back", "Select", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
