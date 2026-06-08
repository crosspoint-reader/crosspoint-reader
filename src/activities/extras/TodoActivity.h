#pragma once
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class TodoActivity final : public Activity {
  struct Item {
    char text[65];
    bool done;
  };

  ButtonNavigator buttonNavigator;
  std::vector<Item> items;
  int selectedIndex = 0;
  bool longPressFired = false;

  void loadItems();
  void saveItems() const;

 public:
  TodoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
