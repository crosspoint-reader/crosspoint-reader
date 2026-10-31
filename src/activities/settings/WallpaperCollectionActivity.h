#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"

// Dedicated scrolling picker: independent of UI themes and of popup item limits.
class WallpaperCollectionActivity final : public UiListActivity {
 public:
  WallpaperCollectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("WallpaperCollection", renderer, mappedInput, /*wantsTouchLongPress=*/true) {}

  void onEnter() override;

 private:
  struct Choice {
    std::string id;
    std::string label;
  };

  int listCount() const override { return static_cast<int>(choices.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  bool handleButtons() override;
  void refreshChoices();
  void confirmRemove(int index);
  const char* headerTitle() const override { return tr(STR_WALLPAPER_COLLECTION); }

  std::vector<Choice> choices;
  std::vector<freeink::ui::ListItem> items;
};
