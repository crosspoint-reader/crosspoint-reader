#pragma once

#include <functional>

#include "MappedInputManager.h"
#include "activities/ActivityWithSubactivity.h"
#include "util/ButtonNavigator.h"

class LineSpacingSelectionActivity final : public ActivityWithSubactivity {
 public:
  explicit LineSpacingSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int initialValue,
                                        const std::function<void(int)>& onSelect, const std::function<void()>& onCancel)
      : ActivityWithSubactivity("LineSpacingSelection", renderer, mappedInput),
        value(initialValue),
        onSelect(onSelect),
        onCancel(onCancel) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  int value = 100;
  ButtonNavigator buttonNavigator;
  const std::function<void(int)> onSelect;
  const std::function<void()> onCancel;

  void adjustValue(int delta);
};
