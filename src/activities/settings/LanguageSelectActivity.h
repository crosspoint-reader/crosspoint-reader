#pragma once

#include <GfxRenderer.h>
#include <I18n.h>

#include <vector>

#include "../Activity.h"
#include "LanguageRegistry.h"
#include "components/UITheme.h"
#include "util/ButtonNavigator.h"

class MappedInputManager;

/**
 * Activity for selecting UI language.
 *
 * Shows all languages discovered at runtime via LanguageRegistry:
 *   - Core (flash-baked): activate immediately.
 *   - Non-core installed (SD YAML): activate immediately.
 *   - Non-core available (manifest): launch LanguageDownloadActivity.
 *   - "More languages..." entry: fetch manifest via LanguageManifestFetchActivity.
 */
class LanguageSelectActivity final : public Activity {
 public:
  explicit LanguageSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("LanguageSelect", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void handleSelection();
  void refreshList();
  int totalListItems() const;

  void onBack() { finish(); }

  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  std::vector<LanguageEntry> _languages;
  bool _hasManifest = false;
};
