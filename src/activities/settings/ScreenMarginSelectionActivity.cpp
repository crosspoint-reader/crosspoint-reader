#include "ScreenMarginSelectionActivity.h"

#include <cstring>
#include <sstream>

#include "CrossPointSettings.h"

ScreenMarginSelectionActivity::ScreenMarginSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                             const std::function<void()>& onBack)
    : ListSelectionActivity(
          "ScreenMarginSelection", renderer, mappedInput, "Select Screen Margin",
          [this]() { return options.size(); },
          [this](size_t index) { return options[index]; },
          [this, onBack](size_t index) {
            if (index >= options.size()) {
              return;
            }
            // Map option index to margin value
            // Options: "5 px", "10 px", "15 px", "20 px", "25 px", "30 px", "35 px", "40 px"
            // Values: 5, 10, 15, 20, 25, 30, 35, 40
            SETTINGS.screenMargin = static_cast<uint8_t>((index + 1) * 5);
            SETTINGS.saveToFile();
            onBack();
          },
          onBack, "No options available") {
  // Initialize options: 5 to 40 in steps of 5
  for (int i = 5; i <= 40; i += 5) {
    std::ostringstream oss;
    oss << i << " px";
    options.push_back(oss.str());
  }
}

void ScreenMarginSelectionActivity::loadItems() {
  // Options are already set in constructor, just set initial selection
  // Map current margin value to option index
  // margin value / 5 - 1 = index (e.g., 5 -> 0, 10 -> 1, etc.)
  if (SETTINGS.screenMargin >= 5 && SETTINGS.screenMargin <= 40) {
    selectorIndex = (SETTINGS.screenMargin / 5) - 1;
    // Ensure index is within bounds
    if (selectorIndex >= options.size()) {
      selectorIndex = 0;  // Default to "5 px"
    }
  } else {
    selectorIndex = 0;  // Default to "5 px"
  }
}
