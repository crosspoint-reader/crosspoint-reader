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
            // Map option index to enum value (index matches enum value)
            SETTINGS.sleepScreen = static_cast<uint8_t>(index);
            SETTINGS.saveToFile();
            onBack();
          },
          onBack, "No options available") {
  // Initialize options from enum
  for (uint8_t i = 0; i < CrossPointSettings::getSleepScreenCount(); i++) {
    options.push_back(CrossPointSettings::getSleepScreenString(i));
  }
}

void SleepScreenSelectionActivity::loadItems() {
  // Options are already set in constructor, just set initial selection
  // Map current enum value to option index
  if (SETTINGS.sleepScreen < options.size()) {
    selectorIndex = SETTINGS.sleepScreen;
  } else {
    selectorIndex = 0;  // Default to "Dark"
  }
}
