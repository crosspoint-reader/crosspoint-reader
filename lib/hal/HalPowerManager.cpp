#include "HalPowerManager.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <soc/soc_caps.h>

#include <cassert>

#include "HalGPIO.h"

#if FREEINK_DEVICE_EEGO_A4 || FREEINK_DEVICE_PICCO
#include <HalFrontlight.h>
#endif

#if FREEINK_DEVICE_PICCO
#include <driver/gpio.h>
#endif

#if FREEINK_DEVICE_PAPERMONO
#include <M5Pm1.h>
#endif

#if SOC_PM_SUPPORT_EXT0_WAKEUP
#include <HalFrontlight.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#endif
#include <esp_rtc_time.h>

HalPowerManager powerManager;  // Singleton instance

// GPIO13 controls the X4 battery latch and the X3 SD power rail on the C3
// Xteink boards. Other boards use it for unrelated signals, including the
// X4 Pro display chip select.
static constexpr gpio_num_t XTEINK_C3_GPIO13 = GPIO_NUM_13;

void HalPowerManager::begin() {
  if (BoardConfig::ACTIVE.batteryAdc >= 0) {
    pinMode(BoardConfig::ACTIVE.batteryAdc, INPUT);
  }
  // Board charger setup (Picco SGM41562: stock charge voltage and limits); a
  // no-op on boards without a configurable charger.
  if (BatteryMonitor::configureCharger()) LOG_INF("PWR", "Charger configured");
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
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

  // Held across the check and the switch: a Lock taken in between would find full speed, do
  // nothing, and then run at LOW_POWER_FREQ until the next key press.
  xSemaphoreTake(modeMutex, portMAX_DELAY);
  const LockMode mode = currentLockMode;

  if (mode == None && enabled && !isLowPower) {
    LOG_DBG("PWR", "Going to low-power mode");
    if (!setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
      xSemaphoreGive(modeMutex);
      return;
    }
    isLowPower = true;

  } else if ((!enabled || mode != None) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      xSemaphoreGive(modeMutex);
      return;
    }
    isLowPower = false;
  }

  // Otherwise, no change needed
  xSemaphoreGive(modeMutex);
}

// Whether the last deep sleep was entered because a press ended a charging hold, and when (RTC time, which
// runs through deep sleep). Such a sleep wakes at once, from the held button or from the plug-in wake while
// the cable still charges, and that wake is the press, so the usual hold check decides it. A wake well after
// it is a real plug-in. RTC_DATA_ATTR survives deep sleep and starts false on a cold boot.
static RTC_DATA_ATTR bool pressEndedHold = false;
static RTC_DATA_ATTR uint64_t pressSleepAtUs = 0;

bool HalPowerManager::lastSleepEndedByPress() {
  constexpr uint64_t PRESS_WAKE_WINDOW_US = 2000000;  // the immediate wake plus boot, with room to spare
  return pressEndedHold && esp_rtc_get_time_us() - pressSleepAtUs < PRESS_WAKE_WINDOW_US;
}

bool HalPowerManager::supportsChargeWake() {
#if SOC_PM_SUPPORT_EXT0_WAKEUP
  // Only the X4 Pro's charge-status line is known well enough: GPIO21, driven HIGH while charging and read
  // with no pull by the stock firmware. Other boards' STAT lines are open-drain or shared, so they stay out.
  const int8_t pin = BoardConfig::ACTIVE.batteryChargeStatus;
  return BoardConfig::isX4Pro() && pin >= 0 && pin != BoardConfig::ACTIVE.input.power &&
         BoardConfig::ACTIVE.input.power >= 0 && rtc_gpio_is_valid_gpio(static_cast<gpio_num_t>(pin));
#else
  return false;
#endif
}

bool HalPowerManager::isCharging() {
  const int8_t pin = BoardConfig::ACTIVE.batteryChargeStatus;
  const bool activeHigh = BoardConfig::ACTIVE.batteryChargeStatusActiveHigh;
  // A weak pull to the idle level, so a line the charger leaves floating reads as not charging.
  pinMode(pin, activeHigh ? INPUT_PULLDOWN : INPUT_PULLUP);
  return (digitalRead(pin) == HIGH) == activeHigh;
}

// Light-sleeps while the battery charges, so an unplug or a press is seen at once (deep sleep would have to boot
// to look). Returns true when the power button ended the hold, false when charging stopped or never ran.
bool HalPowerManager::holdWhileCharging() {
#if SOC_PM_SUPPORT_EXT0_WAKEUP
  if (!isCharging()) return false;
  const auto power = static_cast<gpio_num_t>(BoardConfig::ACTIVE.input.power);
  const bool powerHigh = BoardConfig::ACTIVE.input.powerActiveHigh;
  const int pressed = powerHigh ? HIGH : LOW;
  pinMode(power, powerHigh ? INPUT_PULLDOWN : INPUT_PULLUP);
  // The press that asked for sleep may still be down; it must not end the hold.
  while (digitalRead(power) == pressed) delay(10);
  // The LED PWM can keep running through light sleep.
  Frontlight.setOn(false);
  // Deep sleep isolates every pad; light sleep does not. With the touch rail cut, release the controller's
  // reset and interrupt lines so they cannot back-feed it. The hold always ends in deep sleep and a fresh
  // boot, which sets them up again.
  const auto& touch = BoardConfig::ACTIVE.touch;
  if (touch.powerEnable >= 0) {
    for (const int8_t pin : {touch.reset, touch.irq}) {
      if (pin >= 0) pinMode(pin, INPUT);
    }
  }
  const auto stat = static_cast<gpio_num_t>(BoardConfig::ACTIVE.batteryChargeStatus);
  const bool statHigh = BoardConfig::ACTIVE.batteryChargeStatusActiveHigh;
  LOG_INF("PWR", "Charging: holding in light sleep until unplugged");
  // Charging has stopped only when the line stays idle for a moment: VBUS can bounce or sag through the
  // charger's lockout as a cable comes out, and STAT flickers with it.
  constexpr unsigned long STOP_SETTLE_MS = 500;
  const auto chargingStopped = [] {
    const unsigned long start = millis();
    while (millis() - start < STOP_SETTLE_MS) {
      if (isCharging()) return false;
      delay(5);
    }
    return true;
  };
  while (isCharging() || !chargingStopped()) {
    // Level wakes: a change between the check above and sleep entry wakes straight away.
    gpio_wakeup_enable(stat, statHigh ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
    gpio_wakeup_enable(power, powerHigh ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_light_sleep_start();
    gpio_wakeup_disable(stat);
    gpio_wakeup_disable(power);
    // gpio_wakeup_disable() keeps the level type, and a later pinMode() would enable it as an interrupt that
    // storms while the level holds.
    gpio_set_intr_type(stat, GPIO_INTR_DISABLE);
    gpio_set_intr_type(power, GPIO_INTR_DISABLE);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    if (digitalRead(power) == pressed) {
      delay(20);  // a press, not a glitch
      if (digitalRead(power) == pressed) return true;
    }
  }
#endif
  return false;
}

// Deep sleep also wakes when the line starts charging: a cable was plugged in. If it is already charging, that
// wakes at once, which is right, since a cable is in.
void HalPowerManager::armChargeWake() {
#if SOC_PM_SUPPORT_EXT0_WAKEUP
  const auto g = static_cast<gpio_num_t>(BoardConfig::ACTIVE.batteryChargeStatus);
  const bool activeHigh = BoardConfig::ACTIVE.batteryChargeStatusActiveHigh;
  if (esp_sleep_enable_ext0_wakeup(g, activeHigh ? 1 : 0) != ESP_OK) {
    LOG_ERR("PWR", "Plug-in wake: ext0 on GPIO%d refused", static_cast<int>(g));
    return;
  }
  // ext0 needs the RTC peripherals powered through sleep, and the line held at its idle level by a weak pull,
  // since nothing confirms the charger drives it without a cable. A charging STAT drives past the pull.
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  if (activeHigh) {
    rtc_gpio_pullup_dis(g);
    rtc_gpio_pulldown_en(g);
  } else {
    rtc_gpio_pulldown_dis(g);
    rtc_gpio_pullup_en(g);
  }
#endif
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio, const bool wakeOnHeldButton) const {
#if FREEINK_DEVICE_EEGO_A4 || FREEINK_DEVICE_PICCO
  // The frontlight driver and touch share I2C; turn the light off before touch
  // releases the bus. On Picco this also puts the TMA525C to sleep.
  Frontlight.setOn(false);
  gpio.prepareForDeepSleep();
#endif
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
    // Release any surviving pad hold first: hold_en survives deep sleep via
    // the SDK's deepSleep() (esp_sleep_config_gpio_isolate +
    // gpio_deep_sleep_hold_en), and a held pad silently ignores the drive.
    gpio_hold_dis(XTEINK_C3_GPIO13);
    gpio_set_direction(XTEINK_C3_GPIO13, GPIO_MODE_OUTPUT);
    gpio_set_level(XTEINK_C3_GPIO13, 0);
    gpio_hold_en(XTEINK_C3_GPIO13);
  }
#endif

  // Hold every configured power-latch pin HIGH through deep sleep. These are
  // keep-alive enables (the X4 Pro's master peripheral rail on GPIO1, the
  // Sticky's PWR_HOLD/PWR_LOCK): deepSleep() isolates all pads
  // (esp_sleep_config_gpio_isolate), so a latch without an armed hold loses its
  // output driver and floats — on the X4 Pro the latch drops as soon as
  // external power leaves (serial/pogo adapter unplugged), and the next power-
  // button press cold-boots instead of fast-waking. holdPowerRails() asserted
  // the latches at boot but arms no sleep hold; arm it here instead. Skips
  // XTEINK_C3_GPIO13: it IS power.latch0 on the C3 Xteink boards, where the
  // block above drives it LOW on purpose (battery power-off).
  for (const int8_t pin : {BoardConfig::ACTIVE.power.latch0, BoardConfig::ACTIVE.power.latch1}) {
    if (pin < 0 || static_cast<gpio_num_t>(pin) == XTEINK_C3_GPIO13) continue;
    const auto g = static_cast<gpio_num_t>(pin);
    // Release any surviving pad hold first: a held pad silently ignores the
    // drive below (same trap as the GPIO13 block above).
    gpio_hold_dis(g);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
    gpio_hold_en(g);
  }

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
#if FREEINK_DEVICE_METALIO_EINK4
  // Deep sleep leaves the board, 4G modem and audio module powered, so cut
  // power instead; the button cold-boots it. Returns only on USB power.
  freeink::metalio::powerOff();
#endif

  bool armHeldButton = wakeOnHeldButton;
  if (chargeWake && supportsChargeWake()) {
    // A press that ends the charging hold wakes the device straight back as a power-button wake. The plug-in
    // wake is armed either way, so no path sleeps without it: if the press is let go before sleep, a cable
    // still charging wakes it.
    pressEndedHold = !armHeldButton && holdWhileCharging();
    pressSleepAtUs = esp_rtc_get_time_us();
    armHeldButton = armHeldButton || pressEndedHold;
    armChargeWake();
  } else {
    pressEndedHold = false;
  }

  if (armHeldButton) {
    freeink::PowerManager::armPowerButtonWakeup();
    freeink::PowerManager::deepSleep();
  }
  // Waits for the power button to be physically released (so holding it doesn't
  // immediately wake the device again), then arms the wake source and sleeps.
  freeink::PowerManager::deepSleepUntilPowerButton();
}

#if FREEINK_DEVICE_PICCO
// ponytail: 30 s poll; the SGM41562 status is I2C-only, so USB unplug and
// charge progress are only seen when the timer fires.
static constexpr uint64_t USB_HOLD_POLL_US = 30ULL * 1000 * 1000;

bool HalPowerManager::holdWhileUsbPowered(HalGPIO& gpio, void (*onStatus)(bool charging, uint16_t percent)) const {
  if (!gpio.isUsbConnected()) return false;
  LOG_DBG("PWR", "USB power present, holding in light sleep");
  const auto powerPin = static_cast<gpio_num_t>(BoardConfig::ACTIVE.input.power);
  // The press that requested sleep may still be down; it must not end the hold.
  while (digitalRead(powerPin) == LOW) delay(10);  // Picco power button is active-low
  const BatteryMonitor battery;
  int lastCharging = -1;
  int lastPercent = -1;
  while (gpio.isUsbConnected()) {
    const bool charging = battery.isCharging();
    const uint16_t percent = getBatteryPercentage();
    if (static_cast<int>(charging) != lastCharging || percent != lastPercent) {
      lastCharging = charging;
      lastPercent = percent;
      onStatus(charging, percent);
    }
    esp_sleep_enable_timer_wakeup(USB_HOLD_POLL_US);
    gpio_wakeup_enable(powerPin, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_light_sleep_start();
    gpio_wakeup_disable(powerPin);
    // gpio_wakeup_disable() keeps the LOW_LEVEL type, and a later pinMode() would
    // enable it as an interrupt that storms while the button is held.
    gpio_set_intr_type(powerPin, GPIO_INTR_DISABLE);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (digitalRead(powerPin) == LOW) return true;
  }
  return false;
}
#endif

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
