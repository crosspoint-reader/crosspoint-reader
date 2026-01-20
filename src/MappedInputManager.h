#pragma once

#include <InputManager.h>

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward };

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  explicit MappedInputManager(InputManager& inputManager) : inputManager(inputManager) {}

  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  bool isPressed(Button button) const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  
  // Inject button press from external source (e.g., BLE keyboard)
  void injectButton(Button button);

 private:
  InputManager& inputManager;
  decltype(InputManager::BTN_BACK) mapButton(Button button) const;
};
