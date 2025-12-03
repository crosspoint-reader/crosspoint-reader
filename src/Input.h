#pragma once

// 4 buttons on ADC resistor ladder: Back, Confirm, Left, Right
#define BTN_GPIO1 1
// 2 buttons on ADC resistor ladder: Volume Up, Volume Down
#define BTN_GPIO2 2
// Power button (digital)
#define BTN_GPIO3 3

// Button ADC thresholds
#define BTN_THRESHOLD 100  // Threshold tolerance
#define BTN_RIGHT_VAL 3
#define BTN_LEFT_VAL 1500
#define BTN_CONFIRM_VAL 2700
#define BTN_BACK_VAL 3550
#define BTN_VOLUME_DOWN_VAL 3
#define BTN_VOLUME_UP_VAL 2305

enum Button { NONE = 0, RIGHT, LEFT, CONFIRM, BACK, VOLUME_UP, VOLUME_DOWN, POWER };

struct Input {
  Button button;
  unsigned long pressTime;
};

void setupInputPinModes();
Button getPressedButton();
Input getInput(bool skipWait = false);
