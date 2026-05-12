#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"

class AppCategoryActivity final : public Activity {
 public:
  struct AppEntry {
    const char* nameStrId;
    const char* description;
    UIIcon icon;
    std::function<std::unique_ptr<Activity>(GfxRenderer&, MappedInputManager&)> factory;
    bool isSectionHeader = false;
    std::function<bool()> hasActiveState = nullptr;
  };

  static AppEntry SectionHeader(const char* label) {
    return AppEntry{label, nullptr, UIIcon::File, nullptr, true};
  }

  explicit AppCategoryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* title,
                                std::vector<AppEntry> entries, bool requiresDisclaimer = false, int categoryIndex = -1)
      : Activity("AppCategory", renderer, mappedInput),
        title(title),
        entries(std::move(entries)),
        requiresDisclaimer(requiresDisclaimer),
        categoryIndex(categoryIndex) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  const char* title;
  std::vector<AppEntry> entries;
  bool requiresDisclaimer;
  bool disclaimerShown = false;
  int categoryIndex = -1;

  static constexpr int TILE_COLS = 2;
  static constexpr int TILE_ROWS_VISIBLE = 4;
  static constexpr int TILE_GAP = 6;

  std::vector<int> tileIndices;  // entries[] indices for non-header items
  int selectorIndex = 0;         // index into tileIndices
  int scrollRow = 0;
  bool backPressedHere = false;

  void drawAppTile(int entryIdx, int x, int y, int w, int h, bool selected) const;
  void launchSelected();
};
