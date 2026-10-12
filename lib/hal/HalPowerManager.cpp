#include "HalPowerManager.h"

#include <BatteryMonitor.h>
#include <BoardConfig.h>
#include <HalCapabilities.h>
#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

#include <cassert>

#include "HalGPIO.h"
#include "HalI2cBus.h"

HalPowerManager powerManager;  // Singleton instance

// The C3 flash SPIWP pad, unused in these boards' DIO flash mode and rewired as a
// power control on the Xteink C3 units (see lightSleep()).
static constexpr gpio_num_t GPIO_BATTERY_LATCH = GPIO_NUM_13;

// Whether this build is allowed to call setCpuFrequencyMhz() at all.
//
// On the ESP32-S3 the PSRAM clock is derived from the CPU/APB clock, so changing
// the CPU frequency while anything is touching PSRAM corrupts those accesses. On
// the T5S3 that is not a corner case: the 960x540 framebuffer lives in PSRAM, so
// the render path is in it more or less continuously. It is no better on the X4
// Pro, whose prebuilt Arduino variant sets CONFIG_SPIRAM_USE_MALLOC with
// CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096 — every allocation over 4 KB lands in
// PSRAM — and which runs that PSRAM octal at 80 MHz.
//
// Observed on T5S3 hardware as a TG1WDT_SYS_RST immediately after the "Going to
// low-power mode" line, on four consecutive boots and then intermittently —
// intermittent because it depends on what is mid-access when the switch happens,
// which is exactly the signature of this hazard rather than of a logic bug.
//
// This started life as an #if inside setPowerSaving() alone (commit 0a0851eb),
// which left the two waveform hooks below scaling the clock unguarded. That gap
// was invisible on the T5S3 — its panel is LGFX/parallel and never reaches
// EpdBus::waitBusy, so the hooks never fire there — but on the X4 Pro they fire
// on EVERY refresh (EpdBus fires them above a 20 ms wait), which would have made
// the hot path the one that breaks PSRAM. Hence a single named predicate: every
// setCpuFrequencyMhz() call site in this file is gated on it.
//
// The cost is idle power on a board that has 8 MB of PSRAM and a wired USB port;
// the alternative is random resets. Revisit if ESP-IDF's dynamic frequency
// scaling is ever configured properly for this build (it needs the PSRAM-aware
// DFS path, not a bare setCpuFrequencyMhz()).
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
static constexpr bool CPU_SCALING_ALLOWED = false;
#else
static constexpr bool CPU_SCALING_ALLOWED = true;
#endif

void HalPowerManager::begin() {
  if (HalCapabilities::hasI2cFuelGauge()) {
    // Boards with an I2C fuel gauge read charge over the bus rather than from an
    // ADC divider (X3 has a BQ27220 at 0x55, X4 Pro a CW2017 at 0x63, T5S3 a
    // BQ27220 at 0x55; X4 has none and uses its ADC pin).
    //
    // We do NOT start the bus here. That is HalI2cBus::ensureBusStarted()'s job:
    // the gauge is one peripheral on a shared bus, not its owner, and on a touch
    // board the SDK's InputManager has already started it. main.cpp calls the
    // owner after gpio.begin() so the touch driver gets first claim.
    //
    // The gauge PROTOCOL is no longer ours: getBatteryPercentage() dispatches
    // through BatteryMonitor on ACTIVE.batteryGauge.gaugeType, so a BQ27220
    // (X3, T5S3) and a CW2017 (X4 Pro) each get their own register map. This
    // used to be a hardcoded BQ27220 read, which is the same trap as
    // DS3231-vs-BM8563 in HalClock and failed on the X4 Pro for the same reason.
    _batteryUseI2C = true;
  } else if (HalCapabilities::hasAdcBattery()) {
    pinMode(BoardConfig::ACTIVE.batteryAdc, INPUT);
  }
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }
  if constexpr (!CPU_SCALING_ALLOWED) {
    (void)enabled;
    return;
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }

  // Under modeMutex, as Lock acquisition is. This used to read the lock state relaxed and change the
  // clock after it, on the theory that a Lock which won the race would call setPowerSaving() again.
  // It does not: a Lock calls it once, on acquisition. So a downclock decided on "no lock held" and
  // completed AFTER a Lock's restore stranded that holder at 10 MHz until the next button press --
  // a 190 ms render took 3.2 s, and the loop task waiting on it 4 s (X3, Covers view). With the
  // check and the change under the mutex, a Lock either comes first and is seen, or comes after and
  // finds isLowPower set and restores it.
  if (modeMutex == nullptr) return;
  xSemaphoreTake(modeMutex, portMAX_DELAY);
  const LockMode mode = currentLockMode.load(std::memory_order_relaxed);

  if (mode == None && enabled && !isLowPower) {
    LOG_DBG("PWR", "Going to low-power mode");
    if (setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      isLowPower = true;
    } else {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
    }
  } else if ((!enabled || mode != None) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (setCpuFrequencyMhz(normalFreq)) {
      isLowPower = false;
    } else {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
    }
  }
  // Otherwise, no change needed
  xSemaphoreGive(modeMutex);
}

void HalPowerManager::ensureFullSpeedForRadio() {
  if (normalFreq <= 0) {
    return;  // begin() not called yet — nothing to restore to
  }
  if constexpr (!CPU_SCALING_ALLOWED) {
    return;  // nothing ever lowered the clock, so there is nothing to restore
  }
  // Clear BOTH downclock owners before touching the frequency. The idle governor
  // (isLowPower) and the waveform hook (waveformLowPower_) track their drops separately, and
  // whichever one is live would otherwise restore or re-drop the clock behind the radio's back.
  xSemaphoreTake(modeMutex, portMAX_DELAY);
  const bool wasIdleLow = isLowPower;
  const bool wasWaveformLow = waveformLowPower_;
  isLowPower = false;
  waveformLowPower_ = false;
  xSemaphoreGive(modeMutex);

  const int current = getCpuFrequencyMhz();
  if (current >= normalFreq) {
    return;  // already at full speed; nothing to do beyond the flag reconciliation above
  }
  if (setCpuFrequencyMhz(normalFreq)) {
    LOG_INF("PWR", "CPU %d -> %d MHz for radio bring-up (idleLow=%d waveformLow=%d)", current, normalFreq,
            wasIdleLow ? 1 : 0, wasWaveformLow ? 1 : 0);
  } else {
    // Worth an error rather than a debug line: WiFi association from a 10 MHz clock does not
    // fail cleanly, it hangs, and this is the only place that would have seen it coming.
    LOG_ERR("PWR", "Failed to restore %d MHz before radio bring-up (still %d MHz) — WiFi may hang", normalFreq,
            current);
  }
}

void HalPowerManager::enterWaveformWait() {
  if (normalFreq <= 0) {
    return;  // begin() not called yet — nothing to restore to
  }
  if constexpr (!CPU_SCALING_ALLOWED) {
    // EpdBus fires this hook above a 20 ms busy wait, i.e. on every refresh.
    // Scaling here would be the most frequent PSRAM-clock change in the build.
    return;
  }
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    return;  // WiFi requires the 80 MHz APB clock
  }
  xSemaphoreTake(modeMutex, portMAX_DELAY);
  // A NormalSpeed lock held by ANOTHER task means full-speed code is running
  // concurrently — don't downclock. Our own lock (the render task's per-render
  // Lock) is fine: this task is about to sleep and only resumes after
  // exitWaveformWait() restores the clock.
  //
  // More than one holder means at least one of them is not us, whoever lockOwnerTask_ names —
  // that is the build-slice case, where the loop task is laying out pages while this task waits
  // on a waveform. Treat it as foreign: never downclock on a maybe.
  const bool foreignLock = currentLockMode.load(std::memory_order_relaxed) != None &&
                           (lockCount_ > 1 || lockOwnerTask_ != xTaskGetCurrentTaskHandle());
  // Already at the low clock from idle power saving — leave ownership of the
  // restore with setPowerSaving(); don't double-track it here.
  if (!foreignLock && !isLowPower && setCpuFrequencyMhz(LOW_POWER_FREQ)) {
    waveformLowPower_ = true;
    LOG_TRC("PWR", "Waveform wait: CPU down to %d MHz", LOW_POWER_FREQ);
  }
  xSemaphoreGive(modeMutex);
}

void HalPowerManager::exitWaveformWait() {
  if constexpr (!CPU_SCALING_ALLOWED) {
    return;  // enterWaveformWait() never lowered the clock; stay balanced with it
  }
  xSemaphoreTake(modeMutex, portMAX_DELAY);
  if (waveformLowPower_) {
    waveformLowPower_ = false;
    // If idle power saving engaged meanwhile (loop task), the low clock is now
    // intentional — keep it and let setPowerSaving(false) restore it later.
    if (!isLowPower) {
      setCpuFrequencyMhz(normalFreq);
      LOG_TRC("PWR", "Waveform wait: CPU restored to %d MHz", normalFreq);
    }
  }
  xSemaphoreGive(modeMutex);
}

// Ported from crosspoint-reader PR #2525 (Brian Pugh / @BrianPugh); the Xteink C3
// guard on the GPIO13 hold follows PR #2998 (Justin Mitchell / @itsthisjustin).
// The xTaskCatchUpTicks() correction at the end is this fork's own addition — see
// the comment there for why our background button sampler needs it.
bool HalPowerManager::lightSleep(const HalGPIO& gpio) {
  const unsigned long entryMs = millis();
  if (lastSliceEndMs_ != 0) {
    lightSleepStats_.awakeMs += entryMs - lastSliceEndMs_;
  }
  lightSleepStats_.attempts++;
  lastSliceEndMs_ = entryMs;  // overwritten with the post-sleep stamp if we do sleep

  // Light sleep stops the LEDC peripheral's clock, so an ESP-driven PWM
  // frontlight visibly flickers as the idle loop enters slice after slice.
  // Justin Mitchell (@itsthisjustin) reported this against crosspoint PR #2983
  // ("feat: Add support for x4pro & papermono devices"), where the guard reads
  // `Frontlight.present() && Frontlight.isOn()`. This fork has no frontlight
  // driver, so it cannot ask whether the light is currently lit — decline on any
  // board that HAS one. That costs nothing today (no board we build has a
  // frontlight, so this never fires and the counter stays 0) and keeps the X4
  // Pro correct the moment its profile is selectable. Tighten to
  // present-and-on once a frontlight HAL exists, so those boards can still
  // idle-sleep with the light off.
  if (BoardConfig::hasPwmFrontlight()) {
    lightSleepStats_.rejFrontlight++;
    return false;
  }

  // A performance Lock means a render (or similar) task is mid-flight; light sleep
  // freezes the whole chip, so it would stall that task. Read without the mutex,
  // like setPowerSaving(): stale in either direction is acceptable — a stale lock
  // delays sleeping by one slice, and a Lock acquired after this check freezes that
  // task for at most one slice (timer wake; RAM and peripheral state are retained).
  if (currentLockMode.load(std::memory_order_relaxed) != None) {
    lightSleepStats_.rejLock++;
    return false;
  }
  // Light sleep drops a WiFi association and kills an enumerated USB-CDC link.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    lightSleepStats_.rejWifi++;
    return false;
  }
  if (gpio.isUsbConnectedCached()) {
    lightSleepStats_.rejUsb++;
    return false;
  }
  // A raw button-state change is still inside the debounce window: committing it
  // needs a second matching sample, so the caller should poll again quickly rather
  // than halt the chip — a tap shorter than the slice would otherwise land in a
  // single sample and be dropped, and every press would commit a slice late.
  if (gpio.isDebouncePending()) {
    lightSleepStats_.rejDebounce++;
    return false;
  }

  // Timer wake keeps the poll cadence bounded: the front/side buttons are ADC
  // resistor-ladder inputs whose pressed levels sit mid-rail, invisible to a
  // digital GPIO wake, so they can only be sampled awake. The power button (a
  // true GPIO) is ALSO armed, as a level wake at its pressed level, so a tap
  // shorter than one slice still gets the chip awake in time to sample it. That
  // wake is only an early poll — the sampler's debounce still decides whether a
  // real press happened, so a misread around the sleep transition costs one
  // extra wake, never a phantom press. Idle cost is zero: the pin only holds its
  // pressed level while a finger is on it.
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(LIGHT_SLEEP_SLICE_MS) * 1000ULL);
  const int8_t powerPin = BoardConfig::ACTIVE.input.power;
  if (powerPin >= 0) {
    gpio_wakeup_enable(static_cast<gpio_num_t>(powerPin),
                       BoardConfig::ACTIVE.input.powerActiveHigh ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
  }

  // GPIO13 is rewired as a power control on the Xteink C3 boards: the X4
  // battery-latch MOSFET gate, and the X3 SD-rail enable (BoardConfig's
  // XTEINK_X3 sd.powerEnable). Both are active-high and both must stay HIGH
  // while the device runs. The IDF flash-leakage workaround
  // (CONFIG_ESP_SLEEP_FLASH_LEAKAGE_WORKAROUND) pulls the DIO-unused SPIWP pad
  // low on light-sleep entry, which would power an X4 off outright and cut the
  // X3's card out from under an open file. Drive the pad HIGH and pad-hold it:
  // the hold latches the level and overrides both the sleep-time pull and any
  // pad re-muxing on the wake path. It is left enabled while running — the wake
  // path may restore flash-pad muxing, so even a brief release between slices
  // can drop it. Nothing downstream is blocked by that: both deep-sleep paths
  // release the hold first (startDeepSleep()'s own gpio_hold_dis on X4,
  // holdRailOff()'s inside powerDownRailsForSleep() on X3).
  // Level is set BEFORE direction so the pad never glitches low on the way to
  // output mode. Non-Xteink boards are left alone — GPIO13 is an ordinary signal
  // there (SPI MOSI on the LilyGo T5S3, display CS on the X4 Pro).
  if (gpio.isXteinkDevice()) {
    // gpio_hold_dis() first: a hold left over from an earlier slice (or from a previous
    // sleep cycle) makes gpio_set_level() a silent no-op, so without this the pin would
    // keep whatever level it was latched at — the exact failure mode documented at
    // startDeepSleep()'s own gpio_hold_dis below. Re-holding immediately after is free.
    gpio_hold_dis(GPIO_BATTERY_LATCH);
    gpio_set_level(GPIO_BATTERY_LATCH, 1);
    gpio_set_direction(GPIO_BATTERY_LATCH, GPIO_MODE_OUTPUT);
    gpio_hold_en(GPIO_BATTERY_LATCH);
  }

  const unsigned long sleepStart = millis();
  const esp_err_t err = esp_light_sleep_start();
  const unsigned long sleptMs = millis() - sleepStart;

  // Disarm immediately: an armed timer wake persists across sleep calls and would
  // carry over into startDeepSleep(), waking the device on USB power after one
  // slice. gpio_wakeup_disable() clears only the wake-enable bit — the pin's LEVEL
  // interrupt type survives it, and a leftover level type is live ammunition for
  // any later-installed GPIO ISR service (an asserted level with no per-pin
  // handler re-enters the shared ISR forever). Clear the type explicitly.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (powerPin >= 0) {
    gpio_wakeup_disable(static_cast<gpio_num_t>(powerPin));
    gpio_set_intr_type(static_cast<gpio_num_t>(powerPin), GPIO_INTR_DISABLE);
  }

  if (err != ESP_OK) {
    // e.g. ESP_ERR_SLEEP_REJECT — we did not actually sleep
    lightSleepStats_.rejIdf++;
    LOG_DBG("PWR", "Light sleep rejected: %d", static_cast<int>(err));
    return false;
  }

  lightSleepStats_.slept++;
  lightSleepStats_.sleptMs += sleptMs;
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
    lightSleepStats_.wakeGpio++;  // power button pressed mid-slice
  } else {
    lightSleepStats_.wakeTimer++;
  }
  // Restart the awake stopwatch from here, so awakeMs counts only time the chip
  // was actually running between slices, not the slices themselves.
  lastSliceEndMs_ = millis();

  // esp_light_sleep_start() corrects esp_timer (so millis() stays wall-clock
  // honest) but NOT the FreeRTOS tick, which simply stops for the duration
  // (sleep_modes.c only calls esp_timer_private_set/esp_set_time_from_rtc).
  // Uncorrected, every tick-paced waiter stretches by the slice: the 10 ms
  // button sampler task would only get one pass per ~10 slices, turning idle
  // input latency into half a second, and any delay() elsewhere would run long.
  // Step the tick forward by the measured sleep so blocked tasks come due on
  // wall-clock time. Tasks whose deadline passed are released here, which is why
  // this is xTaskCatchUpTicks() and not vTaskStepTick().
  xTaskCatchUpTicks(pdMS_TO_TICKS(sleptMs));
  return true;
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio, bool keepClockAlive) const {
  LOG_DBG("PWR", "startDeepSleep: isPressed=%d, rawPin=%d, keepClock=%d", gpio.isPressed(HalGPIO::BTN_POWER),
          digitalRead(InputManager::POWER_BUTTON_PIN) == LOW, keepClockAlive);

#ifdef ENABLE_SERIAL_LOG
  // Tear down HWCDC so the host sees a clean disconnect and the peripheral
  // doesn't hold power domains that interfere with USB-powered GPIO wake.
  // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
  // (which doesn't expose end()).
  logSerial.end();
#endif
  // Perform all hardware preparation immediately (while the button may still be held)
  // so the user gets instant visual feedback (display already off). Only block for
  // button release at the very end, right before entering sleep.
  // GPIO13 on X4 is the battery latch MOSFET.
  // When keepClockAlive is false (default): GPIO13 goes LOW, the MCU is
  // completely powered off during sleep (including the LP timer / RTC memory).
  // When keepClockAlive is true: GPIO13 stays HIGH, the MCU remains powered
  // at ~3-4 mA so the LP timer keeps running and RTC memory is preserved.
  // This allows HalClock to accurately compute elapsed sleep time on wake.
  //
  // X3 does NOT share this meaning: there GPIO13 is the SD rail's power enable
  // (active-high), declared as sd.powerEnable in the XTEINK_X3 board profiles.
  // X3 keeps time on a battery-backed DS3231 (I2C 0x68), so it never needs the
  // LP timer held alive across sleep — main.cpp forces keepLpAlive=false on X3
  // for exactly that reason. So on X3 we hand the pin to the SDK's rail teardown
  // (which cuts the card's power and latches it off) instead of treating it as a
  // latch; SDCardManager::begin() releases that hold and re-powers on the next
  // boot. Doing both would make us a second, independent writer of the pin.
  //
  // Non-Xteink boards share neither meaning and take the rail-teardown path too:
  // GPIO13 is an ordinary signal there — SPI MOSI on the LilyGo T5S3
  // (BoardT5S3Pins.h), display chip select on the X4 Pro — so driving it as an
  // output and pad-holding it would clobber a live bus.
  // Those boards also ignore keepClockAlive: nothing cuts MCU power there, so
  // the LP timer and RTC memory survive deep sleep either way.
  // Ported from crosspoint-reader PR #2998 ("fix: Guard GPIO13 power control for
  // Xteink C3 boards only", Justin Mitchell / @itsthisjustin). His fix guards
  // lightSleep()/onEinkBusyWaitSlice(), which this fork does not have; here the
  // same predicate guards the one place we touch GPIO13.
  //
  // Which boards take the latch branch is the profile's call (PowerConfig::latch0GatesBattery,
  // set on the X4 alone), not a board-name test: the X4 Pro's latch0 keeps a rail up instead and
  // is held by the loop further down.
  const bool gpio13IsBatteryLatch = HalCapabilities::hasButtonGatedBatteryLatch();
  if (gpio13IsBatteryLatch) {
    const auto GPIO_SPIWP = static_cast<gpio_num_t>(BoardConfig::ACTIVE.power.latch0);  // GPIO13 on the X4
    // Release any GPIO hold from a previous sleep cycle (keepClockAlive=true leaves GPIO13 held after wake).
    // Without this, gpio_set_level() below silently fails and GPIO13 is stuck in its prior state,
    // causing the device to enter a sleep/wake loop that requires a hardware reset to escape.
    gpio_hold_dis(GPIO_SPIWP);
    gpio_deep_sleep_hold_dis();
    gpio_set_direction(GPIO_SPIWP, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_SPIWP, keepClockAlive ? 1 : 0);
    esp_sleep_config_gpio_isolate();
    gpio_deep_sleep_hold_en();
    gpio_hold_en(GPIO_SPIWP);
  } else {
    // Same release, for the boards that reach GPIO13 through the SDK's rail teardown
    // instead of the latch branch above. It matters on X3: lightSleep() re-holds GPIO13
    // HIGH on every idle slice for ALL Xteink C3 boards (isXteinkDevice()), but only the
    // X4 branch above released it — so powerDownRailsForSleep() could not drive the X3's
    // SD rail enable LOW and the card stayed powered through deep sleep, draining the
    // battery. Guarded to Xteink C3 for the same reason the branch above is: elsewhere
    // GPIO13 is an ordinary bus signal (SPI MOSI on the T5S3, display CS on the X4 Pro)
    // that nothing has held and that we must not touch.
    if (gpio.isXteinkDevice()) {
      gpio_hold_dis(GPIO_NUM_13);
    }
    gpio_deep_sleep_hold_dis();

    // Hold every configured power-latch pin HIGH through deep sleep. These are keep-alive
    // enables — the X4 Pro's master peripheral rail on GPIO1 — and
    // esp_sleep_config_gpio_isolate() below strips the output driver off any pad without an
    // armed hold. holdPowerRails() asserts the latches at boot but arms no sleep hold, so
    // the X4 Pro's latch drops the moment external power leaves and the next power-button
    // press cold-boots (~8-15 s) instead of fast-waking (~1-2 s).
    //
    // Ported from crosspoint-reader PR #3215 ("fix(x4pro): hold power-latch pins HIGH
    // through deep sleep", Antoine Aflalo / @Belphemur), filed against upstream issue #2863.
    //
    // ORDER OF OPERATIONS, and it is not optional: holding the rail up means the panel keeps
    // its VCC through sleep, so the panel MUST actually be parked or its charge pump stays
    // biased and draws mA — the exact regression #3215 exposed. The driver-side half is
    // freeink-sdk 7f541f3, which makes deepSleep() always emit POF before DSLP instead of
    // gating it on an _isScreenOn flag that an AA pass desynchronises. Both halves are in
    // this tree as of the SDK bump to 1be4233; do NOT bring this loop forward past that.
    //
    // The loop is unreachable on the C3 anyway (gpio13IsBatteryLatch takes the branch above
    // on X4, and on X3 latch0 is unassigned), but skip GPIO13 explicitly rather than rely on
    // that: it IS power.latch0 on the C3 Xteink boards, where LOW is a deliberate battery
    // power-off and driving it HIGH here would be the opposite of what the caller asked for.
    for (const int8_t latchPin : {BoardConfig::ACTIVE.power.latch0, BoardConfig::ACTIVE.power.latch1}) {
      if (latchPin < 0 || static_cast<gpio_num_t>(latchPin) == GPIO_BATTERY_LATCH) continue;
      const auto latch = static_cast<gpio_num_t>(latchPin);
      // Release any surviving pad hold first: a held pad silently ignores the drive below,
      // the same trap the GPIO13 branch above documents.
      gpio_hold_dis(latch);
      gpio_set_level(latch, 1);
      gpio_set_direction(latch, GPIO_MODE_OUTPUT);
      gpio_hold_en(latch);
    }

    freeink::PowerManager::powerDownRailsForSleep();
    esp_sleep_config_gpio_isolate();
    gpio_deep_sleep_hold_en();
  }
  pinMode(InputManager::POWER_BUTTON_PIN, INPUT_PULLUP);
  if (sleepStepHook_) sleepStepHook_(SleepStep::RailsConfigured, 0, false, gpio13IsBatteryLatch);

  // Now wait for the power button to be fully released before arming the wakeup
  // trigger and entering sleep — prevents immediate re-wake from a held button.
  // Bounded (see waitForStablePowerRelease): a pin stuck LOW used to freeze the device
  // here with the sleep screen displayed and no watchdog covering this task.
  if (sleepStepHook_) sleepStepHook_(SleepStep::AwaitingRelease, 0, false, gpio13IsBatteryLatch);
  const unsigned long releaseWaitMs = gpio.waitForStablePowerRelease();
  const bool releaseTimedOut = releaseWaitMs >= HalGPIO::POWER_RELEASE_TIMEOUT_MS;
  if (sleepStepHook_) sleepStepHook_(SleepStep::ReleaseDone, releaseWaitMs, releaseTimedOut, gpio13IsBatteryLatch);

  // Arm the wakeup trigger *after* the button is released
  // Note: when keepClockAlive is false, this is only useful for waking up on USB power. On battery, the MCU will be
  // completely powered off, so the power button is hard-wired to briefly provide power to the MCU, waking it up
  // regardless of the wakeup source configuration.
  // When keepClockAlive is true, this is the actual wakeup mechanism since the MCU stays powered.
  //
  // The wake source itself is chip-family specific, so this branches on SoC
  // capability rather than on a board or device name: the C3 has no RTC IO mux
  // and uses the dedicated deep-sleep GPIO path, while the S3 wakes from EXT1
  // over its RTC GPIOs (the X4 Pro power key is GPIO3, which is RTC-capable).
  // Both boards' power keys are active-LOW, matching the level used here.
  constexpr uint64_t powerPinMask = 1ULL << InputManager::POWER_BUTTON_PIN;
#if SOC_PM_SUPPORT_EXT1_WAKEUP
  esp_sleep_enable_ext1_wakeup_io(powerPinMask, ESP_EXT1_WAKEUP_ANY_LOW);
#elif SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
  esp_deep_sleep_enable_gpio_wakeup(powerPinMask, ESP_GPIO_WAKEUP_GPIO_LOW);
#else
#error "No deep-sleep wake source available for this target — add one before enabling this board."
#endif
  if (sleepStepHook_) sleepStepHook_(SleepStep::WakeArmed, releaseWaitMs, releaseTimedOut, gpio13IsBatteryLatch);
  // Enter Deep Sleep
  esp_deep_sleep_start();
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  // Guard against an X3 board mistakenly taking the ADC path: BAT_GPIO0 is
  // reused as X3_I2C_SCL on X3, so reading it as ADC would collide with the
  // fuel-gauge bus. _batteryUseI2C must match the detected device type.
  assert(_batteryUseI2C == HalCapabilities::hasI2cFuelGauge());

  // One monitor for both backends, built from the WHOLE battery config rather
  // than half of it: the default constructor takes the ADC pin, the divider
  // multiplier AND the charge-status pin (with its per-board polarity) from
  // BoardConfig::ACTIVE. The previous `BatteryMonitor(ACTIVE.batteryAdc)` passed
  // the pin and defaulted the rest, which happens to be identical on X3/X4
  // (divider 2.0, no charge-status pin) but silently drops both on any board
  // that differs -- the X4 Pro's active-high STAT line on GPIO21 among them.
  static const BatteryMonitor battery;

  if (_batteryUseI2C) {
    const unsigned long now = millis();
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return _batteryCachedPercent;
    }
    _batteryLastPollMs = now;

    // Ask the SDK monitor, which dispatches on the profile's GaugeType, instead
    // of reading BQ27220 registers by hand.
    //
    // The hand-rolled read was StateOfCharge() at 0x55 -- correct for the X3 and
    // the T5S3, both of which carry a BQ27220, and wrong for the X4 Pro, whose
    // gauge is a CW2017 at 0x63 with an entirely different register map. Nothing
    // answers at 0x55 there, so every poll failed and the reported percentage was
    // whatever the cache last held. Device-confirmed: two
    // `i2cWriteReadNonStop ... ESP_ERR_INVALID_STATE` lines per Home render, one
    // BATTERY_POLL_MS apart. (The error surfaces on the read rather than the
    // address phase because endTransmission(false) defers the transfer under the
    // ESP-IDF i2c_master driver.)
    //
    // The SDK already had the answer: BatteryMonitor carries the CW2017 driver,
    // including the 80-byte BATINFO profile upload the part needs before it will
    // report anything but 0%, and picks the backend from
    // ACTIVE.batteryGauge.gaugeType. Its init is self-throttled to one attempt
    // per second and returns false rather than blocking, so this stays cheap on
    // the loop task.
    //
    // The bus lock is ours to take: BatteryMonitor talks to Wire directly, and on
    // a touch board the GT911 is serviced from the sampler task on the same pins.
    // Same arrangement as HalStorage::usbDriveExternalPower().
    uint16_t soc = 0;
    bool read = false;
    {
      HalI2cBus::Lock i2cLock;
      read = battery.readPercentageChecked(soc);
    }
    // On a failed read keep the last known value, to avoid UI jitter.
    if (read) {
      _batteryCachedPercent = soc > 100 ? 100 : soc;
    }
    return _batteryCachedPercent;
  }
  // ADC path. Only reached when hasI2cFuelGauge() was false, so hasAdcBattery()
  // holds and batteryAdc is a real pin -- the same gate that decides whether
  // gpio.begin() configures it as an input.
  //
  // NOTE _batteryCachedPercent means different things in the two branches: a
  // plain percent above, and percent x10 below. Safe only because a board takes
  // one branch for its whole life.

  // Smooth the battery % with a 1/10-weight IIR. The cache stores the value
  // scaled ×10 so integer math keeps enough precision. Seed explicitly on the
  // first real sample; using 0 as a sentinel caused a second seed whenever a
  // later reading momentarily returned 0, producing visible jumps.
  const uint16_t sample = battery.readPercentage();
  if (!_batterySeeded) {
    _batteryCachedPercent = 10 * sample;
    _batterySeeded = true;
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + sample * 10) / 10;
  }
  return _batteryCachedPercent / 10;
}

namespace {
// BQ27220 standard commands, read only for the before/after log lines below. The
// load itself, and the decision whether one is needed, belong to the SDK.
constexpr uint8_t BQ27220_FULL_CHARGE_CAPACITY_REG = 0x12;
constexpr uint8_t BQ27220_DESIGN_CAPACITY_REG = 0x3C;

void logGaugeCapacity(const char* when, const uint8_t addr, const uint16_t profileMah) {
  uint16_t design = 0;
  uint16_t fullCharge = 0;
  if (X3GPIO::readI2CReg16LE(addr, BQ27220_DESIGN_CAPACITY_REG, &design) &&
      X3GPIO::readI2CReg16LE(addr, BQ27220_FULL_CHARGE_CAPACITY_REG, &fullCharge)) {
    LOG_INF("PWR", "BQ27220 %s: design %u mAh, full-charge %u mAh (profile %u mAh)", when, design, fullCharge,
            profileMah);
  } else {
    LOG_INF("PWR", "BQ27220 %s: capacity read failed", when);
  }
}
}  // namespace

void HalPowerManager::serviceGaugeCapacityLoad() {
  if (!_gaugeCapacityPending) return;

  // Ported from crosspoint-reader PR #3730 ("fix: load the X3 battery capacity into
  // its BQ27220 fuel gauge", Tuan Q. Nguyen / @martinqnguyen), which drives the SDK
  // sequence from Free-Ink/freeink-sdk#132 by the same author. The gauge keeps Design
  // Capacity in RAM: one that lost power comes back at TI's 3000 mAh default and then
  // learns a full-charge capacity for a cell four times the X3's real 650 mAh. The
  // SDK owns the protocol and decides whether anything needs writing; this drives it.
  //
  // Differs from upstream in the lock. They step under a try-RenderLock because their
  // themes read the gauge while drawing. Every gauge access here already goes through
  // HalI2cBus (getBatteryPercentage(), the X3's Current() read in HalGPIO), so that is
  // the lock to hold, and for the whole step: one step can be a block select and its
  // data read 10 ms apart.
  //
  // A deep sleep taken inside the ~8 s window can leave the gauge in CONFIG UPDATE;
  // the SDK's first step on the next boot closes a load left open, with a reinit.
  const auto& gauge = BoardConfig::ACTIVE.batteryGauge;
  if (!_batteryUseI2C || gauge.gaugeType != BoardConfig::GaugeType::Bq27220 || gauge.designCapacityMah == 0) {
    _gaugeCapacityPending = false;
    return;
  }

  const unsigned long now = millis();
  const bool firstCall = _gaugeCapacityStartMs == 0;
  if (firstCall) {
    _gaugeCapacityStartMs = now;
    logGaugeCapacity("at boot", gauge.gaugeAddr, gauge.designCapacityMah);
  }
  {
    HalI2cBus::Lock i2cLock;
    _gaugeCapacityPending = BatteryMonitor::loadDesignCapacity();
  }
  if (_gaugeCapacityPending) return;

  if (firstCall) {
    LOG_INF("PWR", "BQ27220 capacity: nothing to load");
  } else {
    LOG_INF("PWR", "BQ27220 capacity load finished after %lu ms", now - _gaugeCapacityStartMs);
    logGaugeCapacity("after load", gauge.gaugeAddr, gauge.designCapacityMah);
  }
}

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  // Counted, not exclusive: every Lock holds. See lockCount_ for what the old single-slot version
  // silently did to the second holder.
  if (powerManager.lockCount_ < UINT8_MAX) {
    powerManager.lockCount_++;
    valid = true;
  } else {
    LOG_ERR("PWR", "Lock count overflow; this lock holds nothing");  // cannot happen; not silent if it does
    valid = false;
  }
  if (valid && powerManager.lockCount_ == 1) {
    // First holder owns the flag and is the task enterWaveformWait() compares against.
    powerManager.currentLockMode.store(NormalSpeed, std::memory_order_relaxed);
    powerManager.lockOwnerTask_ = xTaskGetCurrentTaskHandle();
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
    // Immediately restore normal CPU frequency if currently in low-power mode. Unconditional, not
    // just for the first holder: a nested lock wants full speed as much as the outer one does.
    powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid && powerManager.lockCount_ > 0 && --powerManager.lockCount_ == 0) {
    powerManager.currentLockMode.store(None, std::memory_order_relaxed);
    powerManager.lockOwnerTask_ = nullptr;
  }
  xSemaphoreGive(powerManager.modeMutex);
}
