#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ClockActivity final : public Activity {
 public:
  explicit ClockActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Clock", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return true; }

 private:
  enum ClockMode { NTP_CLOCK, STOPWATCH_MODE, POMODORO_MODE };
  ClockMode clockMode = NTP_CLOCK;

  ButtonNavigator buttonNavigator;

  // ---- NTP Clock ----
  bool ntpSynced = false;
  bool ntpSyncFailed = false;
  unsigned long ntpLastUpdateMs = 0;
  static constexpr unsigned long NTP_UPDATE_INTERVAL_MS = 60000;
  static const char* dayOfWeekName(int wday);
  static const char* monthName(int mon);
  void ntpSync();

  // ---- Stopwatch ----
  enum SwState { SW_IDLE, SW_RUNNING, SW_STOPPED };
  SwState swState = SW_IDLE;
  unsigned long swStartMs = 0;
  unsigned long swElapsedMs = 0;
  unsigned long swPausedElapsed = 0;
  std::vector<unsigned long> swLaps;
  static constexpr int MAX_LAPS = 10;

  // ---- Pomodoro ----
  enum PomState { POM_IDLE, POM_WORK, POM_SHORT_BREAK, POM_LONG_BREAK };
  PomState pomState = POM_IDLE;
  bool pomPaused = false;
  unsigned long pomRemainingMs = 0;
  unsigned long pomLastTickMs = 0;
  unsigned long pomWorkDurationMs = 25 * 60 * 1000UL;
  unsigned long pomShortBreakMs = 5 * 60 * 1000UL;
  unsigned long pomLongBreakMs = 15 * 60 * 1000UL;
  int pomCompletedPomodoros = 0;
  int pomCycleCount = 0;

  // ---- Shared display throttle ----
  unsigned long lastDisplayMs = 0;
  static constexpr unsigned long DISPLAY_INTERVAL = 100;
  static constexpr unsigned long POM_DISPLAY_INTERVAL = 1000;

  static void formatMs(unsigned long ms, char* buf, size_t len);
  static void formatTime(unsigned long ms, char* buf, size_t bufLen);
  const char* pomStateLabel() const;

  // Render helpers
  void renderNtpClock();
  void renderStopwatch();
  void renderPomodoro();

  // Pomodoro helpers
  void pomStartWork();
  void pomStartShortBreak();
  void pomStartLongBreak();
  void pomResetToIdle();
};
