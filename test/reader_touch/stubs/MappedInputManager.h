#pragma once

#include "util/HomeButtonInput.h"

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, PageBack, PageForward, Power };
  enum class SwipeDir { None, Left, Right, Up, Down };

  bool tap = false;
  int tapX = 0;
  int tapY = 0;
  SwipeDir swipe = SwipeDir::None;

  bool hasTouch() const { return true; }
  bool wasScreenTapped(int& x, int& y) const {
    if (!tap) return false;
    x = tapX;
    y = tapY;
    return true;
  }
  SwipeDir wasSwipe() const { return swipe; }
  bool wasMenuGesture() const { return false; }
  bool wasReaderMenuSwipeUp() const { return false; }
  bool wasBackGesture() const { return false; }
  bool isNavDirectionSwapped() const { return false; }
  bool wasPressed(Button) const { return false; }
  bool wasReleased(Button) const { return false; }
  bool wasLongPressed(Button, unsigned long) const { return false; }
  unsigned long getHeldTime() const { return 0; }
  HomeButtonAction homeButtonAction() const { return HomeButtonAction::Ignore; }
};
