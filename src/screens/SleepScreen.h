#pragma once
#include "Screen.h"

class CrossPointSettings;

class SleepScreen final : public Screen {
 public:
  explicit SleepScreen(GfxRenderer& renderer, InputManager& inputManager, const CrossPointSettings& settings)
      : Screen(renderer, inputManager), settings(settings) {}
  void onEnter() override;

 private:
  const CrossPointSettings& settings;
};
