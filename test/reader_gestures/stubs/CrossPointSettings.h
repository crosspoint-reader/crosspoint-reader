#pragma once

#include <cstdint>

class CrossPointSettings {
 public:
  enum ORIENTATION { PORTRAIT = 0, LANDSCAPE_CW = 1, INVERTED = 2, LANDSCAPE_CCW = 3 };
  enum SHORT_PWRBTN { IGNORE = 0, SLEEP = 1, PAGE_TURN = 2 };
  enum LONG_PRESS_BUTTON_BEHAVIOR { OFF = 0, CHAPTER_SKIP = 1, ORIENTATION_CHANGE = 2 };

  uint8_t longPressButtonBehavior = OFF;
  uint8_t tiltPageTurn = 0;
  uint8_t shortPwrBtn = IGNORE;
  uint8_t backShortToFileBrowser = 0;

  int getRefreshFrequency() const { return 1; }
};

extern CrossPointSettings testSettings;
#define SETTINGS testSettings
