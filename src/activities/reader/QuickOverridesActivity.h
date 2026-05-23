#pragma once
#include <I18n.h>

#include <string>

#include "../MenuListActivity.h"

// Bottom-of-the-toolbox quick-overrides menu: shows only the per-book overrides
// (font family, size, embedded style, image rendering, bionic, paragraph
// alignment, anti-aliasing, hyphenation). Optimised for snappy in-book toggling:
// renders in FAST_REFRESH mode and reports back through MenuResult; the parent
// reader applies the result through its existing applyBookReaderOverrides()
// path, which itself short-circuits when nothing changed.
class QuickOverridesActivity final : public MenuListActivity {
 public:
  QuickOverridesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int8_t initialEmbeddedStyleOverride,
                         int8_t initialImageRenderingOverride, int8_t initialFontFamilyOverride,
                         const std::string& initialSdFontFamilyOverride, int8_t initialFontSizeOverride,
                         int8_t initialBionicReadingOverride, int8_t initialParagraphAlignmentOverride,
                         int8_t initialTextAntiAliasingOverride, int8_t initialHyphenationOverride);

  void onEnter() override;
  void render(RenderLock&&) override;

 private:
  void buildMenuItems();
  void finishWithResult(bool cancelled);

  void onBackPressed() override;
  void onSettingToggled(int index) override;

  int8_t pendingEmbeddedStyleOverride = -1;
  int8_t pendingImageRenderingOverride = -1;
  int8_t pendingFontFamilyOverride = -1;
  std::string pendingSdFontFamilyOverride;
  int8_t pendingFontSizeOverride = -1;
  int8_t pendingBionicReadingOverride = -1;
  int8_t pendingParagraphAlignmentOverride = -1;
  int8_t pendingTextAntiAliasingOverride = -1;
  int8_t pendingHyphenationOverride = -1;
};
