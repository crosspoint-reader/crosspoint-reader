#include "ScreenMarginSelectionActivity.h"

#include <cstring>

#include "CrossPointSettings.h"

ScreenMarginSelectionActivity::ScreenMarginSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                             const std::function<void()>& onBack)
    : ListSelectionActivity(
          "ScreenMarginSelection", renderer, mappedInput, "Select Screen Margin", [this]() { return options.size(); },
          [this](size_t index) { return options[index]; },
          [this, onBack](size_t index) {
            if (index >= options.size()) {
              return;
            }
            SETTINGS.screenMargin = static_cast<uint8_t>(index);
            SETTINGS.saveToFile();
            onBack();
          },
          onBack, "No options available") {
  for (uint8_t i = 0; i < CrossPointSettings::SCREEN_MARGIN_COUNT; i++) {
    options.push_back(CrossPointSettings::getScreenMarginString(i));
  }
}

void ScreenMarginSelectionActivity::loadItems() {
  if (SETTINGS.screenMargin < options.size()) {
    selectorIndex = SETTINGS.screenMargin;
  } else {
    selectorIndex = 0;
  }
}
