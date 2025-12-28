#pragma once
#include "../Activity.h"

class BootActivity final : public Activity {
 public:
  explicit BootActivity(GfxRenderer& renderer, InputManager& inputManager) : Activity("Boot", renderer, inputManager) {}
  void onEnter() override;
};
