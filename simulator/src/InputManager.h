#pragma once
class InputManager {
public:
  InputManager() {}
  void begin() {}
  void update() {}
  bool isPressed(int button) { return false; }
  int getHeldTime(int button) { return 0; }
};
