#pragma once

#include <GfxRenderer.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/UITheme.h"
#include "util/ButtonNavigator.h"

class MappedInputManager;

class ReadingProgressActivity final : public Activity {
 public:
  explicit ReadingProgressActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadingProgress", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct Entry {
    std::string title;
    int percent;  // 0–100
  };

  std::vector<Entry> entries;
  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;

  void loadEntries();
};
