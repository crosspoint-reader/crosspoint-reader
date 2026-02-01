#include "SleepTimeoutSelectionActivity.h"

#include <cstring>

#include "CrossPointSettings.h"

SleepTimeoutSelectionActivity::SleepTimeoutSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                             const std::function<void()>& onBack)
    : ListSelectionActivity(
          "SleepTimeoutSelection", renderer, mappedInput, "Select Time to Sleep", [this]() { return options.size(); },
          [this](size_t index) { return options[index]; },
          [this, onBack](size_t index) {
            if (index >= options.size()) {
              return;
            }
            SETTINGS.sleepTimeout = static_cast<uint8_t>(index);
            SETTINGS.saveToFile();
            onBack();
          },
          onBack, "No options available") {
  for (uint8_t i = 0; i < CrossPointSettings::SLEEP_TIMEOUT_COUNT; i++) {
    options.push_back(CrossPointSettings::getSleepTimeoutString(i));
  }
}

void SleepTimeoutSelectionActivity::loadItems() {
  if (SETTINGS.sleepTimeout < options.size()) {
    selectorIndex = SETTINGS.sleepTimeout;
  } else {
    selectorIndex = 2;  // Default to "10 min" (SLEEP_10_MIN)
  }
}
