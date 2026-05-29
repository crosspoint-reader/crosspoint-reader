#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "activities/Activity.h"
#include "network/GoogleClient.h"

/**
 * RemindersActivity — the Taskpoint screen. Connects to WiFi, pulls Google
 * Calendar + Tasks, renders a countdown task list, and keeps the countdowns live
 * (per-minute FAST_REFRESH) for a five-minute window before going stale and
 * letting the device fall asleep with the list left on the panel.
 *
 * Entered either by a double power-press while awake (main.cpp replaces the
 * current activity) or by the s_syncOnWake flag set before sleeping.
 */
class RemindersActivity final : public Activity {
 public:
  explicit RemindersActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Reminders", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // We manage our own lifetime (live window, then stale + sleep), so suppress
  // the global auto-sleep timeout until the stale banner is up. The failure
  // screen lets auto-sleep run normally so a dead network can't pin us awake.
  bool preventAutoSleep() override { return state != State::Failed && !staleReached; }

 private:
  enum class State { Syncing, Showing, Failed };

  void startSync();
  void onMinuteTick();
  void enterStale();

  static void syncTaskTrampoline(void* param);
  void runSyncTask();

  State state = State::Syncing;
  bool syncStarted = false;
  volatile bool syncDone = false;
  GoogleClient::Result syncResult = GoogleClient::Result::OK;
  TaskHandle_t syncTask = nullptr;

  // Per-minute live window bookkeeping.
  unsigned long showStartMs = 0;
  int tickCount = 0;
  bool tickRefresh = false;  // render() uses FAST_REFRESH when set
  bool staleReached = false;

  // Double-tap Confirm detection (400 ms window) for manual re-sync.
  unsigned long lastConfirmMs = 0;

  static constexpr int LIVE_WINDOW_TICKS = 5;      // five one-minute ticks
  static constexpr unsigned long TICK_MS = 60000;  // one minute
  static constexpr unsigned long DOUBLE_TAP_MS = 400;
};
