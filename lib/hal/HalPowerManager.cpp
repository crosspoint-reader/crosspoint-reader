#include "HalPowerManager.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <soc/soc_caps.h>

#include <cassert>

#include "HalFrontlight.h"
#include "HalGPIO.h"

#if FREEINK_DEVICE_PAPERMONO
#include <M5Pm1.h>
#endif

HalPowerManager powerManager;  // Singleton instance

// GPIO13 must stay high during light sleep on the C3 Xteink boards: it controls
// the X4 battery latch and the X3 SD power rail. Other boards use it for
// unrelated signals, including the X4 Pro display chip select.
static constexpr gpio_num_t XTEINK_C3_GPIO13 = GPIO_NUM_13;

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
    gpio_hold_dis(XTEINK_C3_GPIO13);
    gpio_set_direction(XTEINK_C3_GPIO13, GPIO_MODE_OUTPUT);
    gpio_set_level(XTEINK_C3_GPIO13, 0);
    gpio_hold_en(XTEINK_C3_GPIO13);
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

#if FREEINK_DEVICE_PAPERMONO
  // Its power button is behind the M5PM1 PMIC rather than an ESP GPIO, so
  // normal GPIO deep sleep would have no wake source. Ask the PMIC to shut the
  // device down; a button click then restarts it through a cold boot.
  if (freeink::m5pm1::requestShutdown()) {
    delay(1000);  // allow the PMIC firmware time to drop power
  }
#endif

  // Waits for the power button to be physically released (so holding it doesn't
  // immediately wake the device again), then arms the wake source and sleeps.
  freeink::PowerManager::deepSleepUntilPowerButton();
}

#ifdef CROSSPOINT_TOUCH_INT_WAKE
// A slice that ends this much faster than the stock cadence is treated as an
// instant return rather than a wake: see the busy-loop note in lightSleep().
static constexpr unsigned long IMMEDIATE_RETURN_MS = 20;
#endif

bool HalPowerManager::lightSleep(const HalGPIO& gpio, [[maybe_unused]] const unsigned long maxSliceMs) const {
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
#ifdef FREEINK_FRONTLIGHT_LS
  // The frontlight PWM runs from RC_FAST with KEEP_ALIVE and survives light
  // sleep (SDK FREEINK_FRONTLIGHT_LS), so a lit light no longer blocks it.
  if (WiFi.getMode() != WIFI_MODE_NULL || gpio.isUsbConnectedCached()) {
#else
  // It also stops the default LEDC PWM output, visibly flashing ESP-driven
  // frontlights as the idle loop enters repeated sleep slices.
  if (WiFi.getMode() != WIFI_MODE_NULL || gpio.isUsbConnectedCached() || (Frontlight.present() && Frontlight.isOn())) {
#endif
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
  unsigned long sliceMs = LIGHT_SLEEP_SLICE_MS;
#ifdef CROSSPOINT_TOUCH_INT_WAKE
  // The same argument, widened to the rest of the inputs: with the touch INT
  // and every nav key armed as level wakes too, the timer is no longer what
  // ends an idle slice — the user is. The slice may then run up to the caller's
  // bound, turning twenty short wakes per second into one long window, without
  // costing input latency. The bound is only taken when EVERY wake pin got
  // armed: one that was skipped for sitting at its wake level is input state
  // the poll after this call has not consumed yet (a GT911 INT still held
  // because update() skipped its I2C slot, a button mid-press), and sleeping a
  // full second on it would be a visible stall.
  bool allWakePinsArmed = false;
  if (gpio.inputWakeAvailable() && gpio.armInputWake(allWakePinsArmed) > 0) {
    esp_sleep_enable_gpio_wakeup();
    if (allWakePinsArmed && maxSliceMs > sliceMs) {
      sliceMs = maxSliceMs;
    }
  }
#endif
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(sliceMs) * 1000ULL);
  const int8_t powerPin = BoardConfig::ACTIVE.input.power;
  if (powerPin >= 0) {
    gpio_wakeup_enable(static_cast<gpio_num_t>(powerPin),
                       BoardConfig::ACTIVE.input.powerActiveHigh ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
  }

  // The C3 flash-leakage workaround pulls its DIO-unused SPIWP pad low during
  // light sleep. Hold GPIO13 high only on the X3/X4 boards that use that pad as
  // a power control; on other boards GPIO13 may be a bus signal.
  if (gpio.isXteinkDevice()) {
    gpio_set_level(XTEINK_C3_GPIO13, 1);
    gpio_set_direction(XTEINK_C3_GPIO13, GPIO_MODE_OUTPUT);
    gpio_hold_en(XTEINK_C3_GPIO13);
  }

#ifdef CROSSPOINT_TOUCH_INT_WAKE
  const unsigned long sleepStartMs = millis();
#endif
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
#ifdef CROSSPOINT_TOUCH_INT_WAKE
  // Same reason, for the input pins: they are wake sources for the duration of
  // one slice only, so nothing here can turn a nav key or the touch INT into a
  // deep-sleep wake source (startDeepSleep() always runs outside this call and
  // arms its own).
  gpio.disarmInputWake();
#endif

  xSemaphoreGive(sleepMutex);

  if (err != ESP_OK) {
    // e.g. ESP_ERR_SLEEP_REJECT — we did not actually sleep
    LOG_DBG("PWR", "Light sleep rejected: %d", static_cast<int>(err));
    return false;
  }

#ifdef CROSSPOINT_TOUCH_INT_WAKE
  // Busy-loop floor. A level wake pin that asserts between armInputWake()'s
  // released-check and the sleep start — or the power pin, which is armed above
  // whatever its level — ends the slice immediately. One such round is fine:
  // the poll right after this call consumes it. A line that keeps re-asserting
  // (a GT911 INT stuck low behind an I2C fault, a chattering contact) would
  // otherwise spin the loop at full rate with no sleep at all, the exact
  // opposite of this feature, so anything shorter than IMMEDIATE_RETURN_MS is
  // padded back out to the cadence the stock slice would have had. A plain
  // delay() is safe here: the tick only stops while the chip sleeps, and the
  // other sleeper (the render task's BUSY slice) implies a Lock, which this
  // call already declined on.
  const unsigned long sleptMs = millis() - sleepStartMs;
  if (sleptMs < IMMEDIATE_RETURN_MS) {
    delay(LIGHT_SLEEP_SLICE_MS - sleptMs);
  }
#endif
  return true;
}

bool HalPowerManager::onEinkBusyWaitSlice(const int8_t busyPin, const uint8_t busyLevel) {
  // Same exclusions as lightSleep(). No LOG here — this runs ~50x/s mid-refresh.
#ifdef FREEINK_FRONTLIGHT_LS
  if (WiFi.getMode() != WIFI_MODE_NULL || gpio.isUsbConnectedCached()) {
#else
  if (WiFi.getMode() != WIFI_MODE_NULL || gpio.isUsbConnectedCached() || (Frontlight.present() && Frontlight.isOn())) {
#endif
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

  // Preserve the C3 Xteink GPIO13 power control during this sleep slice without
  // reconfiguring GPIO13 on boards where it is a bus signal.
  if (gpio.isXteinkDevice()) {
    gpio_set_level(XTEINK_C3_GPIO13, 1);
    gpio_set_direction(XTEINK_C3_GPIO13, GPIO_MODE_OUTPUT);
    gpio_hold_en(XTEINK_C3_GPIO13);
  }

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
