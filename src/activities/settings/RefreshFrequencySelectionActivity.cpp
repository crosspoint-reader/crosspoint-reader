#include "RefreshFrequencySelectionActivity.h"

#include <cstring>

#include "CrossPointSettings.h"

RefreshFrequencySelectionActivity::RefreshFrequencySelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                                     const std::function<void()>& onBack)
    : ListSelectionActivity(
          "RefreshFrequencySelection", renderer, mappedInput, "Select Refresh Frequency",
          [this]() { return options.size(); },
          [this](size_t index) { return options[index]; },
          [this, onBack](size_t index) {
            if (index >= options.size()) {
              return;
            }
            // Map option index to enum value (index matches enum value)
            SETTINGS.refreshFrequency = static_cast<uint8_t>(index);
            SETTINGS.saveToFile();
            onBack();
          },
          onBack, "No options available") {
  // Initialize options from enum
  for (uint8_t i = 0; i < CrossPointSettings::REFRESH_FREQUENCY_COUNT; i++) {
    options.push_back(CrossPointSettings::getRefreshFrequencyString(i));
  }
}

void RefreshFrequencySelectionActivity::loadItems() {
  if (SETTINGS.refreshFrequency < options.size()) {
    selectorIndex = SETTINGS.refreshFrequency;
  } else {
    selectorIndex = 3;  // Default to "15 pages" (REFRESH_15)
  }
}
