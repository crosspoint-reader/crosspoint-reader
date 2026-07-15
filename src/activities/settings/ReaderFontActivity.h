#pragma once

#include <SdCardFontRegistry.h>

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

// Combined font family + font size selection with a shared live preview pane.
// Layout: header / preview pane / tab bar (Family | Size) / list / button hints.
// The tab bar is position 0 of the Up/Down navigation ring (same idiom as
// SettingsActivity); Confirm on it switches tab. Confirm on a list item
// previews it; a second Confirm on the previewed item applies both settings
// and exits. Back reverts everything.
class ReaderFontActivity final : public Activity {
 public:
  enum class Tab : uint8_t { Family = 0, Size = 1 };

  ReaderFontActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const SdCardFontRegistry* registry,
                     Tab initialTab = Tab::Family);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void previewFamily(int listIndex);
  void previewSize(int listIndex);
  void switchTab();
  void revertAndExit();
  void renderPreviewPane(int top, int height) const;
  int currentListSize() const;
  // Navigation ring position for the active tab: 0 = tab bar, 1..N = list item N-1.
  int& navIndex();
  int navIndex() const;

  struct FontEntry {
    std::string name;
    bool isBuiltin;
    uint8_t settingIndex;
  };

  struct SizeEntry {
    std::string name;
    uint8_t settingIndex;
  };

  const SdCardFontRegistry* registry_;
  ButtonNavigator buttonNavigator_;
  std::vector<FontEntry> fonts_;
  std::vector<SizeEntry> sizes_;

  Tab tab_;
  int navFamily_ = 1;
  int navSize_ = 1;
  int previewFamilyIndex_ = 0;
  int previewSizeIndex_ = 0;
  int currentFamilyIndex_ = 0;
  int currentSizeIndex_ = 0;

  uint8_t originalFontFamily_ = 0;
  uint8_t originalFontSize_ = 0;
  char originalSdFontFamilyName_[32] = {};

  ThemeMetrics metrics_ = {};
  int afterHeader = 0;
  int bottomReserved = 0;
  int usableHeight = 0;
  int previewHeight = 0;
};
