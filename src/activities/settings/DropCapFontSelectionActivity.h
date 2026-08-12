#pragma once

#include <SdCardFontRegistry.h>

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

// Drop-cap font picker. Mirrors FontSelectionActivity (live preview + list), but
// chooses the enlarged decorative initial face from the standalone /.dropcap
// registry. The first entry is "Default" (no dedicated face — the reader
// integer-scales the body glyph); the rest are the discovered drop-cap families.
class DropCapFontSelectionActivity final : public Activity {
 public:
  explicit DropCapFontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        const SdCardFontRegistry* registry);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void applyPreview(int index);  // load the face for list entry `index` for live preview
  void commitAndFinish();        // persist the selected entry and leave
  // Draws the "Preview "<name>"" label; returns the vertical space it reserves.
  int drawPreviewLabel(int top, int height, const char* fontName) const;
  void renderPreviewPane(int top, int height, int fontId, const char* fontName) const;
  // Truthful "Default" preview: the body glyph integer-scaled exactly as the
  // reader renders a drop cap when no dedicated face is loaded.
  void renderDefaultPreview(int top, int height) const;

  struct Entry {
    std::string name;  // family name, or empty for "Default"
    bool isDefault;
  };

  const SdCardFontRegistry* registry_;
  ButtonNavigator buttonNavigator_;
  std::vector<Entry> entries_;
  int selectedIndex_ = 0;
  int previewIndex_ = 0;
  int previewFontId_ = 0;
  int currentIndex_ = 0;  // entry matching the saved selection (for the "Selected" marker)

  ThemeMetrics metrics_ = {};
  int afterHeader = 0;
  int bottomReserved = 0;
  int usableHeight = 0;
  int previewHeight = 0;
};
