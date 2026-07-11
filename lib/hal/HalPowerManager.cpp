#include "HalPowerManager.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

#include <cassert>

#include "HalGPIO.h"

HalPowerManager powerManager;  // Singleton instance

// GPIO13 is the flash SPIWP pad (unused in this board's DIO flash mode), rewired to the
// battery-latch MOSFET gate: high keeps the battery connected, low powers the device off.
static constexpr gpio_num_t GPIO_BATTERY_LATCH = GPIO_NUM_13;

void HalPowerManager::begin() {
  if (gpio.deviceIsX3()) {
    // X3 uses an I2C fuel gauge for battery monitoring.
    // I2C init must come AFTER gpio.begin() so early hardware detection/probes are finished.
    Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ);
    Wire.setTimeOut(4);
    _batteryUseI2C = true;
  } else {
    pinMode(BAT_GPIO0, INPUT);
  }
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
  // Ensure that the power button has been released to avoid immediately turning back on if you're holding it
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }

#ifdef ENABLE_SERIAL_LOG
  // Tear down HWCDC so the host sees a clean disconnect and the peripheral
  // doesn't hold power domains that interfere with USB-powered GPIO wake.
  // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
  // (which doesn't expose end()).
  logSerial.end();
#endif

  // Pre-sleep routines from the original firmware
  // The battery latch must go low during sleep: the MCU is completely powered off, including RTC
  gpio_hold_dis(GPIO_BATTERY_LATCH);  // lightSleep() holds the pad high; release so we can drive it low
  gpio_set_direction(GPIO_BATTERY_LATCH, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_BATTERY_LATCH, 0);
  esp_sleep_config_gpio_isolate();
  gpio_deep_sleep_hold_en();
  gpio_hold_en(GPIO_BATTERY_LATCH);
  pinMode(InputManager::POWER_BUTTON_PIN, INPUT_PULLUP);
  // Arm the wakeup trigger *after* the button is released
  // Note: this is only useful for waking up on USB power. On battery, the MCU will be completely powered off, so the
  // power button is hard-wired to briefly provide power to the MCU, waking it up regardless of the wakeup source
  // configuration
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  // Enter Deep Sleep
  esp_deep_sleep_start();
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
  // interrupt type survives it, and a leftover level type is live ammunition
  // for any later-installed GPIO ISR service (slice-sleep wedge, candidate #1).
  // Clear the type explicitly.
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

void HalPowerManager::onEinkBusyWaitBegin() {
  if (normalFreq <= 0) {
    return;
  }
  // Same exclusions as setPowerSaving()/lightSleep(): a low clock breaks an
  // active WiFi association and an enumerated USB-CDC link.
  if (WiFi.getMode() != WIFI_MODE_NULL || gpio.isUsbConnectedCached()) {
    return;
  }
  if (setCpuFrequencyMhz(LOW_POWER_FREQ)) {
    busyWaitLowClock = true;
  }
}

void HalPowerManager::onEinkBusyWaitEnd() {
  if (!busyWaitLowClock) {
    return;
  }
  busyWaitLowClock = false;
  if (!setCpuFrequencyMhz(normalFreq)) {
    LOG_ERR("PWR", "Failed to restore CPU frequency after busy wait");
  }
}

bool HalPowerManager::onEinkBusyWaitSlice(const int8_t busyPin, const uint8_t busyLevel) {
  // Same exclusions as lightSleep(): light sleep drops a WiFi association and
  // kills an enumerated USB-CDC link. No LOG here — this runs ~50x/s mid-refresh.
  if (WiFi.getMode() != WIFI_MODE_NULL || gpio.isUsbConnectedCached()) {
    return false;
  }

  xSemaphoreTake(sleepMutex, portMAX_DELAY);

  // Wake the instant BUSY leaves its active level (refresh complete). Level
  // wake (not edge) means an already-completed refresh returns immediately
  // instead of sleeping a full slice. The timer bound only exists so the main
  // loop gets scheduling windows to keep sampling the ADC-ladder buttons.
  const auto pin = static_cast<gpio_num_t>(busyPin);
  gpio_wakeup_enable(pin, busyLevel == HIGH ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
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
  // gpio_wakeup_disable() leaves behind (slice-sleep wedge, candidate #1).
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  gpio_wakeup_disable(pin);
  gpio_set_intr_type(pin, GPIO_INTR_DISABLE);

  xSemaphoreGive(sleepMutex);

  if (err != ESP_OK) {
    return false;  // e.g. ESP_ERR_SLEEP_REJECT — fall back to the SDK's poll delay
  }

  // Yield one tick so the equal-priority main loop can run its input poll:
  // without this the render task re-enters sleep within microseconds and
  // starves button sampling for the entire refresh.
  vTaskDelay(1);
  return true;
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  if (_batteryUseI2C) {
    const unsigned long now = millis();
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return _batteryCachedPercent;
    }

    // Read SOC directly from I2C fuel gauge (16-bit LE register).
    // On I2C error, keep last known value to avoid UI jitter/slowdowns.
    Wire.beginTransmission(I2C_ADDR_BQ27220);
    Wire.write(BQ27220_SOC_REG);
    if (Wire.endTransmission(false) != 0) {
      _batteryLastPollMs = now;
      return _batteryCachedPercent;
    }
    Wire.requestFrom(I2C_ADDR_BQ27220, (uint8_t)2);
    if (Wire.available() < 2) {
      _batteryLastPollMs = now;
      return _batteryCachedPercent;
    }
    const uint8_t lo = Wire.read();
    const uint8_t hi = Wire.read();
    const uint16_t soc = (hi << 8) | lo;
    _batteryCachedPercent = soc > 100 ? 100 : soc;
    _batteryLastPollMs = now;
    return _batteryCachedPercent;
  }
  static const BatteryMonitor battery = BatteryMonitor(BAT_GPIO0);

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
