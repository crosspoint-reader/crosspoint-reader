#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

class FontSizeSelectionActivity final : public Activity {
 public:
  FontSizeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void handleSelection();
  void renderPreviewPane(int top, int height, int fontId, const char* sizeName) const;

  struct FontSizeEntry {
    std::string name;
    uint8_t settingIndex;
  };

  ButtonNavigator buttonNavigator_;
  std::vector<FontSizeEntry> fontSizes_;
  int selectedIndex_ = 0;
  int previewFontSizeIndex_ = 0;
  uint8_t originalFontSize_ = 0;

  ThemeMetrics metrics_ = {};
  int afterHeader = 0;
  int bottomReserved = 0;
  int usableHeight = 0;
  int previewHeight = 0;
};
