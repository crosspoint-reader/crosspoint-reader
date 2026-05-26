#pragma once

#include <Arduino.h>
#include <BoardConfig.h>

class HalBacklight {
 public:
  void begin();
  bool available() const;
  void setBrightness(uint8_t percent);
  uint8_t getBrightness() const { return brightnessPercent; }
  void off() { setBrightness(0); }

 private:
  uint8_t brightnessPercent = 0;
};

extern HalBacklight halBacklight;
