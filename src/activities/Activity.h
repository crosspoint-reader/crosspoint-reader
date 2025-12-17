#pragma once
#include <InputManager.h>

class GfxRenderer;

class Activity {
 protected:
  GfxRenderer& renderer;
  InputManager& inputManager;

 public:
  explicit Activity(GfxRenderer& renderer, InputManager& inputManager)
      : renderer(renderer), inputManager(inputManager) {}
  virtual ~Activity() = default;
  virtual void onEnter() {}
  virtual void onExit() {}
  virtual void loop() {}
};
