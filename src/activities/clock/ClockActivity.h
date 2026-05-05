#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../Activity.h"

/**
 * ClockActivity — displays an NTP-syncable HH:MM clock on the e-ink screen.
 *
 * A background task wakes at each minute boundary, triggers a partial
 * refresh of the digit region, then sleeps until the next boundary.
 * A full refresh fires once per hour to suppress ghosting.
 *
 * Button mapping:
 *   Back / Confirm  → return to Home
 *   PageBack (Up)   → log "wake" sleep-tracker event (M4)
 *   PageForward (Down) → log "sleep" sleep-tracker event (M4)
 */
class ClockActivity final : public Activity {
  TaskHandle_t tickTaskHandle = nullptr;
  static void tickTaskTrampoline(void* param);
  [[noreturn]] void tickTask();

  // Counts full renders since last full-refresh; reset each hour.
  int rendersSinceFullRefresh = 0;
  static constexpr int FULL_REFRESH_INTERVAL = 60;  // once per hour (60 minute renders)

  // Render the clock face; fullRefresh forces a slow full panel clear.
  void renderClock(bool fullRefresh);

 public:
  explicit ClockActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Clock", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
