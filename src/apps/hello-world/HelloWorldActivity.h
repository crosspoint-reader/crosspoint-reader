#pragma once

#include <EInkDisplay.h>
#include <InputManager.h>

class HelloWorldActivity {
 public:
  HelloWorldActivity(EInkDisplay& display, InputManager& input);
  
  void onEnter();
  void loop();
  void onExit();
  
 private:
  EInkDisplay& display_;
  InputManager& input_;
  bool needsUpdate_;
  
  void render();
  void returnToLauncher();
};
