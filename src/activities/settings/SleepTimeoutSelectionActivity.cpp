#include "SleepTimeoutSelectionActivity.h"

#include <cstring>

#include "CrossPointSettings.h"

SleepTimeoutSelectionActivity::SleepTimeoutSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                              const std::function<void()>& onBack)
    : ListSelectionActivity(
          "SleepTimeoutSelection", renderer, mappedInput, "Select Time to Sleep",
          [this]() { return options.size(); },
          [this](size_t index) { return options[index]; },
          [this, onBack](size_t index) {
            if (index >= options.size()) {
              return;
            }
            // Map option index to enum value (index matches enum value)
            SETTINGS.sleepTimeout = static_cast<uint8_t>(index);
            SETTINGS.saveToFile();
            onBack();
          },
          onBack, "No options available") {
  // Initialize options from enum
  for (uint8_t i = 0; i < CrossPointSettings::getSleepTimeoutCount(); i++) {
    options.push_back(CrossPointSettings::getSleepTimeoutString(i));
  }
}

void SleepTimeoutSelectionActivity::loadItems() {
  // Options are already set in constructor, just set initial selection
  // Map current enum value to option index
  if (SETTINGS.sleepTimeout < options.size()) {
    selectorIndex = SETTINGS.sleepTimeout;
  } else {
    selectorIndex = 2;  // Default to "10 min" (SLEEP_10_MIN)
  }
}
