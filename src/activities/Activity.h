#pragma once

#include <HardwareSerial.h>

#include <string>
#include <utility>

class MappedInputManager;
class GfxRenderer;

constexpr int LINE_HEIGHT = 30;

class Activity {
 protected:
  std::string name;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  int marginTop = 15;
  int marginBottom = 50;
  int marginLeft = 20;
  int marginRight = 20;
  int contentStartY = 60;

 public:
  explicit Activity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : name(std::move(name)), renderer(renderer), mappedInput(mappedInput) {}
  virtual ~Activity() = default;
  virtual void onEnter();
  virtual void onExit();
  virtual void loop() {}
  virtual bool skipLoopDelay() { return false; }
  virtual bool preventAutoSleep() { return false; }

  void updateMargins();
  void updateRendererOrientation();

  int getPageItems() const;
};
