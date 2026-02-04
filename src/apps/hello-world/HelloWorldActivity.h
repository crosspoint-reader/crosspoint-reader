#pragma once

#include <HalDisplay.h>
#include <HalGPIO.h>

class HelloWorldActivity {
 public:
  HelloWorldActivity(HalDisplay& display, HalGPIO& input);

  void onEnter();
  void loop();
  void onExit();

 private:
  HalDisplay& display_;
  HalGPIO& input_;
  bool needsUpdate_;

  void render();
  void returnToLauncher();
};
