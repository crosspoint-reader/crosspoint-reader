#include "HalPowerManager.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <soc/soc_caps.h>

#include <cassert>

#include "HalGPIO.h"

HalPowerManager powerManager;  // Singleton instance

// GPIO13 is the flash SPIWP pad (unused in this board's DIO flash mode), rewired to the
// battery-latch MOSFET gate: high keeps the battery connected, low powers the device off.
static constexpr gpio_num_t GPIO_BATTERY_LATCH = GPIO_NUM_13;

void HalPowerManager::begin() {
  if (BoardConfig::ACTIVE.batteryAdc >= 0) {
    pinMode(BoardConfig::ACTIVE.batteryAdc, INPUT);
  }
  mainLoopTask = xTaskGetCurrentTaskHandle();  // Arduino runs setup() on the loop task
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
  sleepMutex = xSemaphoreCreateMutex();
  assert(sleepMutex != nullptr);
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }

  // Note: We don't use mutex here to avoid too much overhead,
  // it's not very important if we read a slightly stale value for currentLockMode
  const LockMode mode = currentLockMode;

  if (mode == None && enabled && !isLowPower) {
    LOG_DBG("PWR", "Going to low-power mode");
    if (!setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
      return;
    }
    isLowPower = true;

  } else if ((!enabled || mode != None) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      return;
    }
    isLowPower = false;
  }

  // Otherwise, no change needed
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
#ifdef ENABLE_SERIAL_LOG
  // Tear down HWCDC so the host sees a clean disconnect and the peripheral
  // doesn't hold power domains that interfere with USB-powered GPIO wake.
  // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
  // (which doesn't expose end()).
  logSerial.end();
#endif

#if !SOC_PM_SUPPORT_EXT1_WAKEUP
  if (gpio.isXteinkDevice()) {
    // GPIO13 gates the battery MOSFET on both Xteink C3 boards; driving it low
    // is the battery power-off (the SDK wake source still handles USB power).
    // Unlike upstream, X3 is included: lightSleep() pad-holds the latch HIGH
    // persistently while running (the IDF flash-leakage workaround would drop
    // it mid-slice, hard-powering the device off), so the implicit low that
    // upstream's X3 relies on never happens here — release the hold and drive
    // the latch low explicitly. The hold_en survives deep sleep via the SDK's
    // deepSleep() (esp_sleep_config_gpio_isolate + gpio_deep_sleep_hold_en).
    gpio_hold_dis(GPIO_BATTERY_LATCH);
    gpio_set_direction(GPIO_BATTERY_LATCH, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_BATTERY_LATCH, 0);
    gpio_hold_en(GPIO_BATTERY_LATCH);
  }
#endif

  // Cut the gated peripheral rails (touch/SD/EPD on boards like the Sticky) and
  // hold the enables off through deep sleep — otherwise the GT911 and SD card
  // stay powered all through "off" and drain the battery. No-op on boards with
  // no switched rails (X4/X3). Trade-off: no touch-to-wake; wake is the power
  // button. Must run after display.deepSleep() so the panel controller gets its
  // deep-sleep command while its rail is still up (enterDeepSleep() in main.cpp
  // guarantees that ordering).
  freeink::PowerManager::powerDownRailsForSleep();

  // Waits for the power button to be physically released (so holding it doesn't
  // immediately wake the device again), then arms the wake source and sleeps.
  freeink::PowerManager::deepSleepUntilPowerButton();
}

bool HalPowerManager::lightSleep(const HalGPIO& gpio) const {
  // A performance Lock means a render (or similar) task is mid-flight; light sleep
  // freezes the whole chip, so it would stall that task.
  // Note: like setPowerSaving(), read without the mutex — stale in either
  // direction is acceptable: a stale lock delays sleeping by one 50 ms slice;
  // a Lock acquired after this check freezes that task for at most one slice
  // (timer wake; RAM/peripheral state retained), which is cheaper than
  // serializing Lock ctor/dtor against the entire sleep window.
  if (currentLockMode != None) {
    return false;
  }
  // Light sleep drops a WiFi association and kills an enumerated USB-CDC link.
  if (WiFi.getMode() != WIFI_MODE_NULL || gpio.isUsbConnectedCached()) {
    return false;
  }

  // Serialize wake-source arming with the render task's BUSY-wait slice (see
  // sleepMutex declaration for the failure mode this prevents).
  xSemaphoreTake(sleepMutex, portMAX_DELAY);

  // Timer wake keeps the button/tilt poll cadence identical to the delay() it
  // replaces: the front/side buttons are ADC resistor-ladder inputs whose
  // pressed levels sit mid-rail, invisible to a digital GPIO wake, so they
  // must be polled. The power button (a true GPIO) is ALSO armed, as a level
  // wake at its pressed level: committing a press needs two update() samples
  // >=5 ms apart, so at the timer-only 50 ms cadence a tap shorter than a
  // slice could land in one sample — or none — and be dropped entirely
  // (field-reported as unreliable short-press sleep). The wake is only an
  // early poll: update()'s debounce still decides whether it was a real
  // press, so a misread around the sleep transition costs one extra wake
  // blip, never a phantom press. Idle cost is zero — the pin only holds its
  // pressed level while a finger is on it.
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(LIGHT_SLEEP_SLICE_MS) * 1000ULL);
  const int8_t powerPin = BoardConfig::ACTIVE.input.power;
  if (powerPin >= 0) {
    gpio_wakeup_enable(static_cast<gpio_num_t>(powerPin),
                       BoardConfig::ACTIVE.input.powerActiveHigh ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
  }

  // The IDF flash-leakage workaround (CONFIG_ESP_SLEEP_FLASH_LEAKAGE_WORKAROUND) pulls the
  // DIO-unused SPIWP pad low on light-sleep entry — on this board that releases the battery
  // latch and hard-powers-off the device. Drive the latch high and pad-hold it (hold latches
  // the pad state and overrides both the sleep-time pull and any pad re-muxing on the wake
  // path). The hold is left enabled permanently while running — the wake path may restore
  // flash-pad muxing, so even a brief release between slices can drop the latch. It is
  // released only in startDeepSleep(), which must drive the latch low to power off.
  // Level is set BEFORE direction so the pad never glitches low on the way to output mode.
  gpio_set_level(GPIO_BATTERY_LATCH, 1);
  gpio_set_direction(GPIO_BATTERY_LATCH, GPIO_MODE_OUTPUT);
  gpio_hold_en(GPIO_BATTERY_LATCH);

  const esp_err_t err = esp_light_sleep_start();

  // Disarm immediately: an armed timer wake persists across sleep calls and would
  // carry over into startDeepSleep(), waking the device on USB power after 50 ms.
  // gpio_wakeup_disable() clears only the wake-enable bit — the pin's LEVEL
  // interrupt type survives it. A leftover level type is live ammunition for
  // any later-installed GPIO ISR service: an asserted level feeding the
  // shared service with no per-pin handler registered re-enters the ISR
  // forever and livelocks the CPU. Clear the type explicitly.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (powerPin >= 0) {
    gpio_wakeup_disable(static_cast<gpio_num_t>(powerPin));
    gpio_set_intr_type(static_cast<gpio_num_t>(powerPin), GPIO_INTR_DISABLE);
  }

  xSemaphoreGive(sleepMutex);

  if (err != ESP_OK) {
    // e.g. ESP_ERR_SLEEP_REJECT — we did not actually sleep
    LOG_DBG("PWR", "Light sleep rejected: %d", static_cast<int>(err));
    return false;
  }
  return true;
}

bool HalPowerManager::onEinkBusyWaitSlice(const int8_t busyPin, const uint8_t busyLevel) {
  // Same exclusions as lightSleep(): light sleep drops a WiFi association and
  // kills an enumerated USB-CDC link. No LOG here — this runs ~50x/s mid-refresh.
  if (WiFi.getMode() != WIFI_MODE_NULL || gpio.isUsbConnectedCached()) {
    return false;
  }

  // Mid-debounce: committing needs a second sample, so let the SDK's short poll
  // delay run instead of halting the chip. Same guard lightSleep() gets in loop().
  // Unsynchronized like the checks above; stale costs at most one slice.
  if (gpio.isDebouncePending()) {
    return false;
  }

  xSemaphoreTake(sleepMutex, portMAX_DELAY);

  // Wake the instant BUSY leaves its active level (refresh complete). Level
  // wake (not edge) means an already-completed refresh returns immediately
  // instead of sleeping a full slice. The timer bound only exists so the main
  // loop gets scheduling windows to keep sampling the ADC-ladder buttons.
  const auto pin = static_cast<gpio_num_t>(busyPin);
  gpio_wakeup_enable(pin, busyLevel == HIGH ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
  // Also wake the instant the power button is pressed, so the main loop's poll
  // sees even sub-slice taps mid-refresh (same rationale and safety argument
  // as lightSleep(): the wake is an early poll, never a synthesized press).
  const int8_t powerPin = BoardConfig::ACTIVE.input.power;
  if (powerPin >= 0) {
    gpio_wakeup_enable(static_cast<gpio_num_t>(powerPin),
                       BoardConfig::ACTIVE.input.powerActiveHigh ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
  }
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(BUSY_SLEEP_SLICE_MS) * 1000ULL);

  // Battery-latch hold: the IDF flash-leakage workaround would drop the latch
  // pad on sleep entry and hard-power-off the device (see lightSleep()).
  gpio_set_level(GPIO_BATTERY_LATCH, 1);
  gpio_set_direction(GPIO_BATTERY_LATCH, GPIO_MODE_OUTPUT);
  gpio_hold_en(GPIO_BATTERY_LATCH);

  const esp_err_t err = esp_light_sleep_start();

  // Disarm everything armed above; an armed source persisting into
  // startDeepSleep() would wake the device on USB power (see lightSleep()).
  // As in lightSleep(): also clear the LEVEL interrupt types, which
  // gpio_wakeup_disable() leaves behind (see the livelock note there).
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  gpio_wakeup_disable(pin);
  gpio_set_intr_type(pin, GPIO_INTR_DISABLE);
  if (powerPin >= 0) {
    gpio_wakeup_disable(static_cast<gpio_num_t>(powerPin));
    gpio_set_intr_type(static_cast<gpio_num_t>(powerPin), GPIO_INTR_DISABLE);
  }

  xSemaphoreGive(sleepMutex);

  if (err != ESP_OK) {
    return false;  // e.g. ESP_ERR_SLEEP_REJECT — fall back to the SDK's poll delay
  }

  // Yield until the equal-priority main loop finishes an iteration, not just one
  // tick: one tick is less awake CPU than an iteration needs, so we would re-halt
  // the chip mid-poll. Capped so a loop parked on SD I/O can't hold the panel awake.
  const uint32_t seenIterations = mainLoopIterations;
  // Skipped entirely when the loop cannot make progress and the yield would just
  // burn the cap awake: parked in requestUpdateAndWait() on this very render;
  // never started (setup-time paints, iterations still 0); or this task IS the
  // loop task (direct displayBuffer calls from setup()/loop() busy-wait here).
  const bool loopCanPoll = !mainLoopBlocked && seenIterations != 0 && xTaskGetCurrentTaskHandle() != mainLoopTask;
  unsigned yieldTicks = 0;
  if (loopCanPoll) {
    for (; yieldTicks < SLICE_YIELD_MAX_TICKS && mainLoopIterations == seenIterations; ++yieldTicks) {
      vTaskDelay(1);
    }
  }
  return true;
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  static const BatteryMonitor battery;
  if (BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0) {
    const unsigned long now = millis();
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return _batteryCachedPercent;
    }

    _batteryLastPollMs = now;
    uint16_t percent = 0;
    if (!battery.readPercentageChecked(percent)) {
      return _batteryCachedPercent;
    }
    _batteryCachedPercent = percent;
    return _batteryCachedPercent;
  }

  // smooth the battery %.
  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * battery.readPercentage();
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + battery.readPercentage() * 10) / 10;
  }
  return _batteryCachedPercent / 10;
}

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  // Current limitation: only one lock at a time
  if (powerManager.currentLockMode != None) {
    LOG_ERR("PWR", "Lock already held, ignore");
    valid = false;
  } else {
    powerManager.currentLockMode = NormalSpeed;
    valid = true;
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
    // Immediately restore normal CPU frequency if currently in low-power mode
    powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid) {
    powerManager.currentLockMode = None;
  }
  xSemaphoreGive(powerManager.modeMutex);
}
