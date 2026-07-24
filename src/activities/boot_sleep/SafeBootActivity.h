#pragma once

#include "activities/Activity.h"

class SafeBootActivity final : public Activity {
 public:
  SafeBootActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool manual)
      : Activity("SafeBoot", renderer, mappedInput), manual_(manual) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  bool manual_;
};
