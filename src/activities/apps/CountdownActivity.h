#pragma once
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class CountdownActivity final : public Activity {
 public:
  explicit CountdownActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Countdown", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == RUNNING; }

 private:
  enum State { SET_TIME, RUNNING, PAUSED, FINISHED };
  State state = SET_TIME;

  int hours = 0, minutes = 30, seconds = 0;
  unsigned long targetTime = 0;
  unsigned long pauseRemaining = 0;
  int editField = 1;  // 0=hours, 1=minutes, 2=seconds

  // Original duration for FINISHED display
  int origHours = 0, origMinutes = 30, origSeconds = 0;

  unsigned long lastDisplayMs = 0;
  static constexpr unsigned long DISPLAY_INTERVAL = 1000;

  void getRemainingHMS(int& h, int& m, int& s) const;
  void drawLargeDigit(int x, int y, int digit, int segW, int segH, bool invert) const;
  void drawLargeTime(int centerX, int y, int h, int m, int s, int highlightField) const;
};
