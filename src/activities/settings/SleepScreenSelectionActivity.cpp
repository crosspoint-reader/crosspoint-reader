#include "SleepScreenSelectionActivity.h"

#include <cstring>

#include "CrossPointSettings.h"

SleepScreenSelectionActivity::SleepScreenSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           const std::function<void()>& onBack)
    : ListSelectionActivity(
          "SleepScreenSelection", renderer, mappedInput, "Select Sleep Screen",
          [this]() { return options.size(); },
          [this](size_t index) { return options[index]; },
          [this, onBack](size_t index) {
            if (index >= options.size()) {
              return;
            }
            SETTINGS.sleepScreen = static_cast<uint8_t>(index);
            SETTINGS.saveToFile();
            onBack();
          },
          onBack, "No options available") {
  for (uint8_t i = 0; i < CrossPointSettings::SLEEP_SCREEN_MODE_COUNT; i++) {
    options.push_back(CrossPointSettings::getSleepScreenString(i));
  }
}

void SleepScreenSelectionActivity::loadItems() {
  if (SETTINGS.sleepScreen < options.size()) {
    selectorIndex = SETTINGS.sleepScreen;
  } else {
    selectorIndex = 0;  // Default to "Dark"
  }
}
