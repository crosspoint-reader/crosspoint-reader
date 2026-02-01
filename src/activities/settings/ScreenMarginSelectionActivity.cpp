#include "ScreenMarginSelectionActivity.h"

#include <cstring>

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
            SETTINGS.screenMargin = CrossPointSettings::SCREEN_MARGIN_VALUES[index];
            SETTINGS.saveToFile();
            onBack();
          },
          onBack, "No options available") {
  for (uint8_t i = 0; i < CrossPointSettings::SCREEN_MARGIN_COUNT; i++) {
    options.push_back(CrossPointSettings::getScreenMarginString(i));
  }
}

void ScreenMarginSelectionActivity::loadItems() {
  const int idx = CrossPointSettings::getScreenMarginIndex(SETTINGS.screenMargin);
  if (idx >= 0 && static_cast<size_t>(idx) < options.size()) {
    selectorIndex = static_cast<size_t>(idx);
  } else {
    selectorIndex = 0;
  }
}
