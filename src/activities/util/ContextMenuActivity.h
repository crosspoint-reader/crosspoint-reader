#pragma once

#include <I18n.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// A small modal menu drawn as an overlay on top of the activity that launched it.
// The launching activity's last frame is left in the framebuffer; this activity only
// paints a centred box, so the underlying screen stays visible around it.
//
// Selecting an item returns a MenuResult{action} to the caller; Back cancels
// (ActivityResult::isCancelled == true). The caller defines the meaning of `action`.
class ContextMenuActivity final : public Activity {
 public:
  struct Item {
    StrId labelId;
    int action;
  };

  ContextMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                      std::vector<Item> items);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string title;
  std::vector<Item> items;
  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;
  // True when Confirm was still held as this overlay opened; swallow that release so we
  // don't immediately activate the first item.
  bool lockNextConfirmRelease = false;
};
