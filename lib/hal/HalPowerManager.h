#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>
#include <Logging.h>
#include <Wire.h>
#include <freertos/semphr.h>

#include <cassert>

#include "HalGPIO.h"

class HalPowerManager;
extern HalPowerManager powerManager;  // Singleton

class HalPowerManager {
  int normalFreq = 0;  // MHz
  bool isLowPower = false;

  // I2C fuel gauge configuration for X3 battery monitoring
  bool _batteryUseI2C = false;                   // True if using I2C fuel gauge (X3), false for ADC (X4)
  mutable int _batteryCachedPercent = 0;         // Last read battery percentage (0-100)
  mutable unsigned long _batteryLastPollMs = 0;  // Timestamp of last battery read in milliseconds

  enum LockMode { None, NormalSpeed };
  LockMode currentLockMode = None;
  SemaphoreHandle_t modeMutex = nullptr;  // Protect access to currentLockMode
  // Bumped by the main loop, read by the render task (see onEinkBusyWaitSlice).
  // Single writer, naturally aligned: the 32-bit access is atomic on RISC-V.
  volatile uint32_t mainLoopIterations = 0;

  // Cap on how long one slice may stay awake waiting for that iteration.
  // Productive yields measure ~0.4 ms, so two ticks is ample headroom.
  static constexpr unsigned SLICE_YIELD_MAX_TICKS = 2;

  // Set while the main loop is blocked in requestUpdateAndWait(). Captured in
  // begin(), which Arduino runs on the loop task, so the handle compare below
  // ignores any other task that might wait on a render.
  TaskHandle_t mainLoopTask = nullptr;
  volatile bool mainLoopBlocked = false;

  // Serializes wake-source arm -> esp_light_sleep_start() -> disarm across the
  // two sleepers (main-loop lightSleep() and the render task's BUSY-wait slice).
  // Without it, lightSleep()'s deliberately unsynchronized lock-mode read could
  // let both tasks interleave arming/disarming: the loser's disarm-all can strip
  // the winner's wake sources and leave it sleeping with none armed.
  SemaphoreHandle_t sleepMutex = nullptr;

 public:
  static constexpr int LOW_POWER_FREQ = 10;  // MHz

  // Two-stage idle backoff. Renders re-raise the clock via Lock regardless, so the
  // full-speed window only needs to cover rapid consecutive input (avoids clock
  // thrash); polling stays at 100 Hz until light sleep takes over the cadence.
  static constexpr unsigned long IDLE_DOWNCLOCK_MS = 500;     // full speed -> LOW_POWER_FREQ
  static constexpr unsigned long IDLE_LIGHT_SLEEP_MS = 1000;  // 100 Hz polling -> light sleep

  static constexpr unsigned long BATTERY_POLL_MS = 1500;     // ms
  static constexpr unsigned long LIGHT_SLEEP_SLICE_MS = 50;  // ms

  // Timer bound for one BUSY-wait light-sleep slice. The refresh end itself
  // wakes exactly via GPIO level wake on the BUSY pin; the timer only bounds
  // how coarse main-loop button sampling gets during a long refresh.
  static constexpr unsigned long BUSY_SLEEP_SLICE_MS = 20;  // ms

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);

  // Setup wake up GPIO and enter deep sleep
  // Should be called inside main loop() to handle the currentLockMode
  void startDeepSleep(HalGPIO& gpio) const;

  // Light-sleep the CPU for LIGHT_SLEEP_SLICE_MS (timer wake; buttons are polled on
  // wake at the same cadence as the delay() this replaces). Returns false WITHOUT
  // sleeping when unsafe: a performance Lock is held (render in flight), WiFi is
  // active, or USB is connected (light sleep kills the CDC link). The caller must
  // fall back to delay() in that case.
  bool lightSleep(const HalGPIO& gpio) const;

  // BUSY-wait slice hook (EpdBus::setBusyWaitSliceHook): light-sleep the chip
  // for up to BUSY_SLEEP_SLICE_MS while the panel refreshes autonomously,
  // waking early the moment the BUSY pin leaves `busyLevel`. Returns false
  // WITHOUT sleeping when unsafe (WiFi active, USB connected, or sleep
  // rejected) — the SDK then falls back to its plain poll delay. Called from
  // the render task; safe by construction (the waiter IS the locked task).
  // Leaves the CPU clock alone: the chip is asleep for most of the wait, so a
  // downclock buys nothing and only slows the main loop's awake windows.
  bool onEinkBusyWaitSlice(int8_t busyPin, uint8_t busyLevel);

  // Call at the end of every main-loop iteration; paces the slice hook's yield.
  // Read-then-assign, not ++: C++20 deprecates increment on a volatile lvalue.
  void noteMainLoopIteration() { mainLoopIterations = mainLoopIterations + 1; }

  // Bracket a blocking wait on the render task (requestUpdateAndWait). While the
  // main loop is parked there it cannot poll, so the slice hook skips its yield
  // instead of burning the full cap awake once per slice for nothing.
  void noteRenderWaitBegin() {
    if (xTaskGetCurrentTaskHandle() == mainLoopTask) mainLoopBlocked = true;
  }
  void noteRenderWaitEnd() {
    if (xTaskGetCurrentTaskHandle() == mainLoopTask) mainLoopBlocked = false;
  }

  // Get battery percentage (range 0-100)
  uint16_t getBatteryPercentage() const;

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for example when running a task that needs
  // full performance. When the Lock instance is destroyed (goes out of scope), power saving will be re-enabled.
  class Lock {
    friend class HalPowerManager;
    bool valid = false;

   public:
    explicit Lock();
    ~Lock();

    // Non-copyable and non-movable
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };
};
