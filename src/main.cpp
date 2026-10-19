#include <Arduino.h>
#include <BoardConfig.h>
#include <CooperativeAbort.h>
#include <Epub.h>
#include <Epub/BookFonts.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalCapabilities.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalI2cBus.h>
#include <HalPowerManager.h>
#include <HalSpiBus.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
// Board-support layer, linked only into the LilyGo env (see its lib_deps).
#if defined(FREEINK_DEVICE_LILYGO) && FREEINK_DEVICE_LILYGO
#include <BoardT5S3.h>
#endif
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <WiFi.h>
#include <builtinFonts/all.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <hal/usb_serial_jtag_ll.h>

#include <cstring>
#include <vector>

#include "BootDiagnostics.h"
#include "ButtonEventManager.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "GestureEventManager.h"
#include "GlobalBookmarkIndex.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "ReadingSessionTracker.h"
#include "ReadingStats.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "SerialTestApi.h"
#include "SilentRestart.h"
#include "UiFontScale.h"
#include "WeatherSettingsStore.h"
#include "WifiCredentialStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/reader/KOReaderAutoSync.h"
#include "activities/reader/KOReaderSyncWorker.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/LoadingIcon.h"
#include "platform/UsbSerialJtagHandoff.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"
#include "util/SerialLineBuffer.h"
#include "util/Timezones.h"
#include "util/WakeTrace.h"

#ifdef ENABLE_BOOT_HEAP_DIAGNOSTICS
#include <BootHeapProbe.h>
// Static-init heap probes bracketing this TU's globals (slots 4/5); see BootHeapProbe.h.
static BootHeapProbe s_probeMainFirst(4);
#endif
// Declared before mappedInputManager: the mapper holds a renderer reference for
// the live-orientation touch transform, and globals in one TU are constructed in
// declaration order.
GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
ButtonEventManager buttonEventManager(mappedInputManager);
GestureEventManager gestureEventManager(mappedInputManager, renderer);

// Lets lib-layer long tasks (image decoders) bail out mid-work so a queued button
// press is serviced on the next main-loop pass. Installed once in setup().
static bool hasPendingButtonInput() { return mappedInputManager.hasPendingInput(); }

ButtonEventManager& globalButtonEvents() { return buttonEventManager; }
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts(), renderer.getSdCardFontAliases());

// Fonts
EpdFont bookerly14RegularFont(&bookerly_14_regular);
EpdFont bookerly14BoldFont(&bookerly_14_bold);
EpdFont bookerly14ItalicFont(&bookerly_14_italic);
EpdFont bookerly14BoldItalicFont(&bookerly_14_bolditalic);
EpdFontFamily bookerly14FontFamily(&bookerly14RegularFont, &bookerly14BoldFont, &bookerly14ItalicFont,
                                   &bookerly14BoldItalicFont);
EpdFont bookerly10RegularFont(&bookerly_10_regular);
EpdFont bookerly10BoldFont(&bookerly_10_bold);
EpdFont bookerly10ItalicFont(&bookerly_10_italic);
EpdFont bookerly10BoldItalicFont(&bookerly_10_bolditalic);
EpdFontFamily bookerly10FontFamily(&bookerly10RegularFont, &bookerly10BoldFont, &bookerly10ItalicFont,
                                   &bookerly10BoldItalicFont);
EpdFont bookerly12RegularFont(&bookerly_12_regular);
EpdFont bookerly12BoldFont(&bookerly_12_bold);
EpdFont bookerly12ItalicFont(&bookerly_12_italic);
EpdFont bookerly12BoldItalicFont(&bookerly_12_bolditalic);
EpdFontFamily bookerly12FontFamily(&bookerly12RegularFont, &bookerly12BoldFont, &bookerly12ItalicFont,
                                   &bookerly12BoldItalicFont);
EpdFont bookerly16RegularFont(&bookerly_16_regular);
EpdFont bookerly16BoldFont(&bookerly_16_bold);
EpdFont bookerly16ItalicFont(&bookerly_16_italic);
EpdFont bookerly16BoldItalicFont(&bookerly_16_bolditalic);
EpdFontFamily bookerly16FontFamily(&bookerly16RegularFont, &bookerly16BoldFont, &bookerly16ItalicFont,
                                   &bookerly16BoldItalicFont);
EpdFont bookerly18RegularFont(&bookerly_18_regular);
EpdFont bookerly18BoldFont(&bookerly_18_bold);
EpdFont bookerly18ItalicFont(&bookerly_18_italic);
EpdFont bookerly18BoldItalicFont(&bookerly_18_bolditalic);
EpdFontFamily bookerly18FontFamily(&bookerly18RegularFont, &bookerly18BoldFont, &bookerly18ItalicFont,
                                   &bookerly18BoldItalicFont);

EpdFont notosans10RegularFont(&notosans_10_regular);
EpdFont notosans10BoldFont(&notosans_10_bold);
EpdFont notosans10ItalicFont(&notosans_10_italic);
EpdFont notosans10BoldItalicFont(&notosans_10_bolditalic);
EpdFontFamily notosans10FontFamily(&notosans10RegularFont, &notosans10BoldFont, &notosans10ItalicFont,
                                   &notosans10BoldItalicFont);
EpdFont notosans12RegularFont(&notosans_12_regular);
EpdFont notosans12BoldFont(&notosans_12_bold);
EpdFont notosans12ItalicFont(&notosans_12_italic);
EpdFont notosans12BoldItalicFont(&notosans_12_bolditalic);
EpdFontFamily notosans12FontFamily(&notosans12RegularFont, &notosans12BoldFont, &notosans12ItalicFont,
                                   &notosans12BoldItalicFont);
EpdFont notosans14RegularFont(&notosans_14_regular);
EpdFont notosans14BoldFont(&notosans_14_bold);
EpdFont notosans14ItalicFont(&notosans_14_italic);
EpdFont notosans14BoldItalicFont(&notosans_14_bolditalic);
EpdFontFamily notosans14FontFamily(&notosans14RegularFont, &notosans14BoldFont, &notosans14ItalicFont,
                                   &notosans14BoldItalicFont);
EpdFont notosans16RegularFont(&notosans_16_regular);
EpdFont notosans16BoldFont(&notosans_16_bold);
EpdFont notosans16ItalicFont(&notosans_16_italic);
EpdFont notosans16BoldItalicFont(&notosans_16_bolditalic);
EpdFontFamily notosans16FontFamily(&notosans16RegularFont, &notosans16BoldFont, &notosans16ItalicFont,
                                   &notosans16BoldItalicFont);
EpdFont notosans18RegularFont(&notosans_18_regular);
EpdFont notosans18BoldFont(&notosans_18_bold);
EpdFont notosans18ItalicFont(&notosans_18_italic);
EpdFont notosans18BoldItalicFont(&notosans_18_bolditalic);
EpdFontFamily notosans18FontFamily(&notosans18RegularFont, &notosans18BoldFont, &notosans18ItalicFont,
                                   &notosans18BoldItalicFont);

EpdFont smallFont(&notosans_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui10RegularFont(&inter_ui_10_regular);
EpdFont ui10BoldFont(&inter_ui_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&inter_ui_12_regular);
EpdFont ui12BoldFont(&inter_ui_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

// Only reachable through the LARGE step of the UI font ladder (see applyUiFontScale()).
EpdFont ui14RegularFont(&inter_ui_14_regular);
EpdFont ui14BoldFont(&inter_ui_14_bold);
EpdFontFamily ui14FontFamily(&ui14RegularFont, &ui14BoldFont);

// SilentRestart.h definitions. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
// Whether the light was actually lit when a silent reboot was armed.
//
// A silent reboot is the firmware's own decision, so the device must come back looking
// exactly as it left -- and "was the light on" is a question only the hardware can answer.
// SETTINGS.frontlightOn cannot: JsonSettingsIO saves it as a stored PREFERENCE that
// deliberately survives a trip to a board with no light, so after a wake that left the light
// off (Restore Light on Wake disabled) it still reads 1 while the panel is dark. Reading it
// on the silent path turned the light on unasked at the next maintenance reboot.
RTC_NOINIT_ATTR uint32_t silentRebootLightOn;
RTC_NOINIT_ATTR uint32_t heapRecoveryRestartLatch;
// Single-shot latch for the boot-time heap integrity recovery restart.
// Prevents an infinite reset loop if the heap is still corrupt after one clean restart.
RTC_NOINIT_ATTR uint32_t heapCorruptionBootLatch;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;
// Boot straight back into the USB serial file-transfer activity. Armed (without
// restarting) while that activity is open, so the unavoidable C3 USB-Serial/JTAG
// reset that fires when a host opens the port lands back in the activity instead
// of Home — making the transfer reset-tolerant. See armSerialTransferReboot().
constexpr uint32_t SILENT_REBOOT_TARGET_SERIAL_TRANSFER = 2;
// Boot into the clock settings screen after a timezone-detection WiFi session
// (WiFi teardown fragments the heap; need a clean reboot before re-entering the UI).
constexpr uint32_t SILENT_REBOOT_TARGET_CLOCK_SETTINGS = 3;
// Sleep was requested while the display framebuffers were released (web server
// session frees both for WiFi heap). SleepActivity cannot render without a
// framebuffer, so reboot to reestablish it and finish the sleep transition right
// after setup(). Two targets so fromTimeout survives the reboot (it gates
// "Quick Resume on Timeout").
constexpr uint32_t SILENT_REBOOT_TARGET_SLEEP = 4;
constexpr uint32_t SILENT_REBOOT_TARGET_SLEEP_TIMEOUT = 5;
// Boot into the KOReader settings screen after an auth/register WiFi session, so
// the user lands back where they started instead of on Home.
constexpr uint32_t SILENT_REBOOT_TARGET_KOREADER_SETTINGS = 6;
// Boot into Settings after a settings action left the radio up on its way out
// (the WiFi Networks picker, Weather's city search), so the user lands back where
// they started instead of on Home.
constexpr uint32_t SILENT_REBOOT_TARGET_SETTINGS = 7;
// Boot into File Transfer, on its OPDS Browser row, after Back left an OPDS catalog
// session, so the user lands where they opened the browser instead of on Home.
constexpr uint32_t SILENT_REBOOT_TARGET_FILE_TRANSFER_OPDS = 8;
// Upper bound for the cold-boot sanity check on silentRebootTarget (RTC_NOINIT is
// uninitialized on power-up). Must equal the highest target above — keep it in
// sync when adding one, or the new target silently reads as HOME.
constexpr uint32_t SILENT_REBOOT_TARGET_MAX = SILENT_REBOOT_TARGET_FILE_TRANSFER_OPDS;
constexpr uint32_t HEAP_RECOVERY_RESTART_LATCH_MAGIC = 0x48EA9C01;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,       // cold boot, flash, panic, or plain reboot
  Silent,       // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  QuickResume,  // wake from a quick-resume deep sleep (SD flag; survives power loss)
  // Wake from a plain deep sleep that is heading straight back into the reader. Distinct from
  // QuickResume because it paints NOTHING at boot: the X3's one mandatory post-begin() full
  // sync (~2.3 s) is reserved for the page itself rather than spent on an interstitial. There
  // is no saved frame on this path — enterDeepSleep() only persists one for Quick Resume.
  ReaderResume,
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

// Arms the RTC flags setup() reads back after a silent reboot. Single entry point so a new
// restart target cannot forget to capture the live light state along with the destination.
static void armSilentReboot(const uint32_t target) {
  silentRebootTarget = target;
  silentRebootLightOn = Frontlight.isOn() ? 1u : 0u;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
}

void silentRestart() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  // ESP.restart() bypasses activity onExit(), so flush any in-flight reading
  // session manually — otherwise a heap-defrag reboot mid-read loses the session.
  globalReadingSessionTracker().end();
  armSilentReboot(SILENT_REBOOT_TARGET_HOME);
  LOG_DBG("MAIN", "Silent restart (target=home)");
  delay(50);
  ESP.restart();
}

void silentRestartToReader() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  globalReadingSessionTracker().end();
  armSilentReboot(SILENT_REBOOT_TARGET_READER);
  LOG_DBG("MAIN", "Silent restart (target=reader)");
  delay(50);
  ESP.restart();
}

void silentRestartToClockSettings() {
  if (deepSleepInProgress) return;
  globalReadingSessionTracker().end();
  armSilentReboot(SILENT_REBOOT_TARGET_CLOCK_SETTINGS);
  LOG_DBG("MAIN", "Silent restart (target=clock-settings)");
  delay(50);
  ESP.restart();
}

void silentRestartToKOReaderSettings() {
  if (deepSleepInProgress) return;
  globalReadingSessionTracker().end();
  armSilentReboot(SILENT_REBOOT_TARGET_KOREADER_SETTINGS);
  LOG_DBG("MAIN", "Silent restart (target=koreader-settings)");
  delay(50);
  ESP.restart();
}

void silentRestartToSettings() {
  if (deepSleepInProgress) return;
  globalReadingSessionTracker().end();
  armSilentReboot(SILENT_REBOOT_TARGET_SETTINGS);
  LOG_DBG("MAIN", "Silent restart (target=settings)");
  delay(50);
  ESP.restart();
}

void silentRestartToFileTransferOpds() {
  if (deepSleepInProgress) return;
  globalReadingSessionTracker().end();
  armSilentReboot(SILENT_REBOOT_TARGET_FILE_TRANSFER_OPDS);
  LOG_DBG("MAIN", "Silent restart (target=file-transfer-opds)");
  delay(50);
  ESP.restart();
}

bool trySilentRestartToReaderForHeapRecovery() {
  if (deepSleepInProgress) return false;  // sleeping supersedes the heap-defrag reboot
  if (heapRecoveryRestartLatch == HEAP_RECOVERY_RESTART_LATCH_MAGIC) {
    LOG_ERR("MAIN", "Heap-recovery restart suppressed by safety latch");
    return false;
  }
  heapRecoveryRestartLatch = HEAP_RECOVERY_RESTART_LATCH_MAGIC;
  globalReadingSessionTracker().end();
  armSilentReboot(SILENT_REBOOT_TARGET_READER);
  LOG_ERR("MAIN", "Silent restart (target=reader, heap recovery)");
  delay(50);
  ESP.restart();
  return true;
}

// Restart as part of an in-progress sleep transition: setup() reestablishes the
// framebuffers, then routes straight back into enterDeepSleep(). No
// deepSleepInProgress guard — this restart IS the sleep path, not a competing
// heap-defrag reboot.
static void silentRestartToSleep(bool fromTimeout) {
  globalReadingSessionTracker().end();
  armSilentReboot(fromTimeout ? SILENT_REBOOT_TARGET_SLEEP_TIMEOUT : SILENT_REBOOT_TARGET_SLEEP);
  LOG_INF("MAIN", "Silent restart (target=sleep, framebuffers released, fromTimeout=%d)", fromTimeout ? 1 : 0);
  delay(50);
  ESP.restart();
}

void restartToHomeAfterStorageHandoff() {
  if (deepSleepInProgress) return;  // sleeping supersedes the storage-handoff reboot
  globalReadingSessionTracker().end();
  armSilentReboot(SILENT_REBOOT_TARGET_HOME);
  LOG_DBG("MAIN", "Restart after storage handoff (target=home)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  // Give the shared USB PHY back to Serial/JTAG before resetting, or the host
  // is left enumerating an OTG device that the reboot destroys.
  handoffUsbOtgToSerialJtag();
  ESP.restart();
}

void armSerialTransferReboot() {
  // Does NOT restart — just arms the RTC target so that *if* the device resets
  // (the C3 hardware reset that fires when a host opens the USB serial port),
  // setup() routes straight back into the serial-transfer activity. Re-armed on
  // every entry into that activity (setup() read-and-clears the magic).
  armSilentReboot(SILENT_REBOOT_TARGET_SERIAL_TRANSFER);
}

void disarmSerialTransferReboot() {
  // Clear the arm on a clean exit so the next plain reboot shows Home as usual.
  if (silentRebootMagic == SILENT_REBOOT_MAGIC && silentRebootTarget == SILENT_REBOOT_TARGET_SERIAL_TRANSFER) {
    silentRebootMagic = 0;
    silentRebootTarget = 0;
  }
}

// ---- Retained-frame persistence across deep sleep ----
//
// Deep sleep loses the framebuffer (and the SDK's previous-frame copy) with the power domain, so
// anything the wake path wants to composite onto must be persisted to SD first.
//
// Only Quick Resume saves one. It keeps the last reader page on screen during deep sleep (just
// overlaid with a moon icon) and restores that frame on wake, so reading resumes with no visible
// boot at all — the user is looking at their own page for the whole load.
//
// A plain reader resume deliberately saves NOTHING, because painting a restored frame would
// consume the X3's one mandatory post-wake full sync on an interstitial and push the page behind
// a second refresh (see enterDeepSleep). The presence of this file is therefore also what setup()
// uses to tell the two resume flows apart.
constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";

static void saveSleepFrameBuffer() {
  FsFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) {
    return;
  }
  const size_t bufferSize = renderer.getBufferSize();
  const size_t written = file.write(renderer.getFrameBuffer(), bufferSize);
  file.close();
  if (written != bufferSize) {
    LOG_DBG("MAIN", "Quick Resume frame save short write: %u/%u", static_cast<unsigned>(written),
            static_cast<unsigned>(bufferSize));
    Storage.remove(SLEEP_FRAME_FILE);
  }
}

// Restores the previously saved framebuffer into the display buffer. Returns false if the file is
// missing or the size does not match — in that case the caller should fall back to a boot screen.
// The file is always removed (success or failure) so a future cold boot does not see a stale frame.
static bool loadSleepFrameBuffer() {
  FsFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) {
    return false;
  }
  const size_t bufferSize = display.getBufferSize();
  const size_t fileSize = file.size();
  const int bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  Storage.remove(SLEEP_FRAME_FILE);
  if (fileSize != bufferSize || bytesRead < 0 || static_cast<size_t>(bytesRead) != bufferSize) {
    LOG_DBG("MAIN", "Quick Resume frame size mismatch (file=%u, read=%d, expected=%u)", static_cast<unsigned>(fileSize),
            bytesRead, static_cast<unsigned>(bufferSize));
    return false;
  }
  return true;
}

// Earliest millis() value at which a held-power-button press is allowed to trigger sleep.
// Set near the end of setup() so a wake-press held a bit too long does not bounce straight
// back into deep sleep before the user sees the page.
static unsigned long allowSleepAt = 0;

// ---- Leftover wake-press guard ---------------------------------------------------------
// The wake press is usually STILL HELD when setup() finishes. Nothing on screen confirms
// that the gesture was accepted (a reader resume deliberately paints nothing until the page
// lands — see BootResume::ReaderResume), so the user keeps holding "to be sure".
//
// setup() used to answer that by blocking on gpio.waitForStablePowerRelease(). That charged
// the user's finger straight to the boot: measured on X4, the `rel` phase cost 1290 ms and
// 3050 ms on two long-hold wakes, against 200 ms — the stability tail alone — on a wake the
// user had already released. Same firmware, same book: 6.8 s to the page versus 3.9 s. The
// hold was not slow to *register*, it was slow because holding is what stalled the boot.
//
// So boot no longer waits. This latch swallows the leftover press from the main loop
// instead, which preserves the one thing the wait was for: the release bounce (10-50 ms on
// these switches, well past InputManager's 5 ms debounce) must not reach the press-type FSM
// as a second click, which on a device with double-click-to-sleep configured would put it
// straight back to sleep.
//
// Serviced by serviceBootPowerRelease(), defined below BootDiag::markPhase().
static bool bootPowerReleasePending = false;
static unsigned long bootPowerHighSince = 0;

static void logStartupMemory(const char* stage) {
  const uint32_t freeHeap = esp_get_free_heap_size();
  const uint32_t minFree = esp_get_minimum_free_heap_size();
  const uint32_t free8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  const uint32_t largest8bit = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  const uint32_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const uint32_t largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

  LOG_INF("MEM", "Startup[%s] free=%lu min=%lu 8bit=%lu largest8=%lu internal=%lu largestInternal=%lu", stage,
          static_cast<unsigned long>(freeHeap), static_cast<unsigned long>(minFree),
          static_cast<unsigned long>(free8bit), static_cast<unsigned long>(largest8bit),
          static_cast<unsigned long>(freeInternal), static_cast<unsigned long>(largestInternal));
}

// The boot phase trace, the wake-gate verdict and the sleep breadcrumb all live in
// BootDiagnostics now, so BootDiagnosticsActivity can render them on device — a reporter
// without a serial monitor could not read any of it before, and a battery-powered device
// never even opens the log wire. Only the BootPhase name is pulled in unqualified: it
// appears at fourteen call sites through setup() and reads better unqualified there.
using BootDiag::BootPhase;

// Poll the raw power pin and keep the boot press away from the event system until it has
// been released for RELEASE_STABLE_MS. See the latch's declaration for why boot no longer
// blocks on this. Call once per loop tick, after ButtonEventManager has drained the sampler
// queue and before any power handling.
static void serviceBootPowerRelease() {
  if (!bootPowerReleasePending) return;
  // Same window waitForStablePowerRelease() used; the raw pin is read directly for the same
  // reason it did — the 5 ms debounce is too short for mechanical release bounce.
  constexpr unsigned long RELEASE_STABLE_MS = 200;
  const unsigned long now = millis();
  if (digitalRead(InputManager::POWER_BUTTON_PIN) == HIGH) {
    if (bootPowerHighSince == 0) bootPowerHighSince = now;
  } else {
    bootPowerHighSince = 0;
  }
  // Unconditional, including on the tick that clears the latch: drain() resets every FSM and
  // flushes the sampler's edge queue plus the pending press/release bits, so neither the boot
  // press, its release, nor the bounce that follows can be seen by loop() or by an activity.
  // It also keeps gpio.wasPressed(BTN_POWER) false, which is what stops the hold-to-sleep
  // timer in loop() from ever starting on the wake press.
  buttonEventManager.drain();
  if (bootPowerHighSince != 0 && now - bootPowerHighSince >= RELEASE_STABLE_MS) {
    bootPowerReleasePending = false;
    // Stamped here rather than in setup(), so `rel` in the boot trace still reports how long
    // the user kept holding past the point the device was ready — it just no longer costs
    // that time. Read it back with CMD:BOOTLOG, which now prints it after the fact.
    BootDiag::markPhase(BootPhase::PowerRelease);
    LOG_DBG("MAIN", "Wake press released at %lu ms (boot was not blocked on it)", now);
  }
}

// Whether GPIO13 (the X4 battery latch) stays HIGH through deep sleep, keeping the MCU
// powered at ~3-4 mA so the LP timer keeps running and RTC memory survives.
//
// Every path that sleeps must agree on this. It used to be computed only inside
// enterDeepSleep(), so the two early-boot "go straight back to sleep" paths took
// startDeepSleep()'s keepClockAlive=false default instead: one short tap on a sleeping
// device (or any spurious wake) was enough to have the wake gate reject the press and put
// the device back down with the battery latch CUT, converting "asleep, wakes on a tap"
// into "powered off, needs a hold long enough to carry the whole boot" — and losing the
// clock with it, because those paths also never called HalClock::saveBeforeSleep().
//
// Any board with a battery-backed RTC is excluded: it keeps time independently, so it
// never needs the LP timer held alive, and paying deep-sleep current to preserve one is
// pure waste. This was `!gpio.deviceIsX3()`, i.e. "the X3 is the only board with an RTC"
// — true while the X3 and X4 were the only two, and wrong the moment a third arrived.
// The X4 Pro's BM8563 and the T5S3's PCF8563 both answer yes here; on the X3 the answer
// is unchanged (DS3231), so the C3 keeps its existing behaviour byte for byte.
static bool keepClockAliveForSleep() { return SETTINGS.useClock && !HalCapabilities::hasHardwareRtc(); }

// Translate HalPowerManager's report of its own last steps into breadcrumb stages. The
// HAL cannot call BootDiag directly (it must not depend on app code), and startDeepSleep()
// does not return, so this is the only point at which "we got as far as arming the wake
// source" can be recorded at all.
static void onSleepStep(HalPowerManager::SleepStep step, unsigned long waitedMs, bool timedOut, bool storageLive) {
  using Step = HalPowerManager::SleepStep;
  switch (step) {
    case Step::RailsConfigured:
      BootDiag::markSleepStage(BootDiag::SleepStage::RailsConfigured);
      break;
    case Step::AwaitingRelease:
      BootDiag::markSleepStage(BootDiag::SleepStage::AwaitingRelease);
      break;
    case Step::ReleaseDone:
      BootDiag::noteReleaseWait(waitedMs, timedOut);
      // Persist ONLY on a timeout. The breadcrumb cannot carry this: on an X4 sleeping
      // with the latch cut the rail is gone before the next boot, and the C3 cannot tell a
      // reset press from a power-on, so without this the pathological case leaves no trace
      // at all. A timeout is also the one safe moment to write — the device is provably
      // still executing, whereas a clean release drops the rail immediately and a write
      // into a collapsing rail is how SD cards lose their FAT.
      if (timedOut) {
        BootDiag::persistReleaseTimeout(storageLive);
      }
      break;
    case Step::WakeArmed:
      BootDiag::markSleepStage(BootDiag::SleepStage::WakeArmed);
      break;
  }
}

// Enter deep sleep mode. fromTimeout=true marks an auto-sleep (gates "Quick Resume on
// Timeout"). `trigger` is diagnostics only — it does not change behaviour, it just records
// which gesture asked for the sleep, which is the first thing a "it did not wake up" report
// needs pinned down.
void enterDeepSleep(bool fromTimeout = false, BootDiag::SleepTrigger trigger = BootDiag::SleepTrigger::PowerHold) {
  LOG_DBG("MAIN", "enterDeepSleep called at millis=%lu, powerBtn isPressed=%d, rawPin=%d", millis(),
          gpio.isPressed(HalGPIO::BTN_POWER), digitalRead(InputManager::POWER_BUTTON_PIN) == LOW);
  // The web server activity releases BOTH framebuffers for WiFi heap
  // (releaseFrameBuffers() nulls the renderer's pointer). SleepActivity would
  // render the sleep screen into nullptr and crash, so reboot instead: setup()
  // reallocates the buffers and routes straight back here to finish the sleep.
  if (renderer.getFrameBuffer() == nullptr) {
    silentRestartToSleep(fromTimeout);
    return;  // not reached — silentRestartToSleep() restarts the chip
  }
  BootDiag::beginSleep(fromTimeout ? BootDiag::SleepTrigger::Timeout : trigger, keepClockAliveForSleep(),
                       activityManager.isReaderActivity());
  // Stop the background sampler before tearing down the display/power rails so no
  // ADC read races with that teardown. From here sleep prep reads the power pin
  // directly (HalGPIO::startDeepSleep / waitForStablePowerRelease).
  gpio.stopInputSampler();
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();
  const bool keepLpAlive = keepClockAliveForSleep();
  HalClock::saveBeforeSleep(keepLpAlive);
  // If sleeping from a running reader the book loaded successfully, so the boot-loop
  // guard count is no longer needed. Reset it now because onExit() is never called
  // on the reader activity during a sleep transition (only queued as a pending action).
  if (APP_STATE.lastSleepFromReader) {
    APP_STATE.readerActivityLoadCount = 0;
  }

  const bool isQuickResumeSleep =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);
  // Sleeping from a book with a book still open means the next boot's destination is already
  // known: setup() will route straight back into the reader. Paint NOTHING before that page.
  //
  // The reason is specific to the panel, not to taste. After begin() the X3 driver arms exactly
  // one mandatory full sync (Uc8253X3Driver: _initialFullSyncsRemaining = 1) — the controller's
  // DTM1 baseline was wiped to white by the power-down, so the first refresh must drive every
  // pixel whatever mode it asks for. Measured on X3: ~2.3 s. Whichever frame paints first
  // consumes it, and everything after it is a cheap differential.
  //
  // So an intermediate frame is not "a cheap extra refresh" — it spends the single expensive
  // sync on a throwaway image and forces the page to pay a second refresh on top. Both the boot
  // splash and a loading-icon-over-sleep-screen do exactly that (measured: wake 4.8 s, of which
  // 2.4 s was the intermediate paint). Skipping straight to the page lets the page itself
  // consume the mandatory sync, removing a whole refresh from the wake.
  //
  // The trade-off, deliberately taken: for the ~700 ms the book takes to open there is no
  // on-screen acknowledgement of the wake press. The panel is not blank — it still physically
  // holds the sleep screen — so this reads as "not woken yet" rather than as a fault.
  const bool resumeIntoReader = APP_STATE.lastSleepFromReader && !APP_STATE.openEpubPath.empty();
  APP_STATE.showBootScreen = !(isQuickResumeSleep || resumeIntoReader);

  APP_STATE.saveToFile();
  BootDiag::markSleepStage(BootDiag::SleepStage::StatePersisted);
  bool keepRadioUp = false;
#if CROSSPOINT_KOREADER_AUTOSYNC
  // Deiniting esp_wifi under a worker job's open socket crashes so let's drain first.
  constexpr unsigned long AUTO_SYNC_PRESLEEP_DRAIN_MS = 3000;
  keepRadioUp = !KOReaderSyncWorker::drain(AUTO_SYNC_PRESLEEP_DRAIN_MS);
  keepRadioUp = keepRadioUp || (KOReaderAutoSync::sleepPushEnabled() && activityManager.isReaderActivity());
#endif
  // Tear down WiFi so the modem power domain isn't held alive across deep sleep.
  // Wake from deep sleep is effectively a chip reset, so no state needs to survive.
  // cppcheck-suppress knownConditionTrueFalse ; compile-time false without CROSSPOINT_KOREADER_AUTOSYNC
  if (!keepRadioUp && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
  // a WiFi activity would otherwise silentRestart() here and reboot instead.
  deepSleepInProgress = true;
  activityManager.goToSleep(fromTimeout);

#if CROSSPOINT_KOREADER_AUTOSYNC
  if (!KOReaderSyncWorker::isBusy() && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
#endif

  // Persist the moon-icon-overlaid framebuffer after goToSleep() has painted it. Quick Resume
  // only: it restores this frame and paints a loading icon over it, which is worth the mandatory
  // full sync because the restored frame IS the reader page — the user is looking at their book
  // for the whole load, not at an interstitial. A plain reader resume saves nothing and paints
  // nothing, so the page gets that sync instead (see showBootScreen above).
  if (isQuickResumeSleep) {
    saveSleepFrameBuffer();
  }

  BootDiag::markSleepStage(BootDiag::SleepStage::ScreenPainted);
  halTiltSensor.deepSleep();

  display.deepSleep();
  BootDiag::markSleepStage(BootDiag::SleepStage::PanelAsleep);
  // Persist before the last two steps, which are the ones that can strand the device:
  // the release wait and the wake arming. Everything that could still touch the SD card
  // is done, so this is the last safe moment to write. The record is amended in place on
  // the next boot when the RTC breadcrumb survived — see BootDiag::persistBoot().
  BootDiag::persistSleep();
  // After the last write of the whole sleep sequence, and only there: this ends
  // the card session, so anything writing afterwards would find no card. Leaves
  // a still-powered card (the T5S3 has no SD rail to cut) idle in a state it
  // defines instead of mid-transaction with a dirty cache.
  Storage.prepareForSleep();
  // Park the reading light for real. off() only writes a zero duty and leaves
  // the pad owned by LEDC, which sleep entry then floats — what the light does
  // after that is down to an external pull. Driven and latched at the inactive
  // level instead, because a light left lit is the largest load a sleeping
  // reader can carry. Done after display.deepSleep() so the panel keeps its
  // light for the whole sleep-screen paint. No-op without a frontlight.
  Frontlight.prepareForDeepSleep();
  LOG_DBG("MAIN", "Entering deep sleep (powerBtn isPressed=%d, rawPin=%d)", gpio.isPressed(HalGPIO::BTN_POWER),
          digitalRead(InputManager::POWER_BUTTON_PIN) == LOW);

  powerManager.startDeepSleep(gpio, keepLpAlive);
}

// The UI font ladder. Each logical slot holds the next family up at the LARGE step; the ladder
// stays inside Inter (plus Noto Sans 8 at the bottom of the default step) on purpose, because
// every alternative built-in face is a COMPRESSED 2-bit font. UI screens do not prewarm the
// glyph cache the way the reader does, so a compressed UI font would inflate a whole DEFLATE
// group per glyph on every menu repaint — the regression that stalled crosspoint-reader#3083.
//
// Line heights are 23/25/30 px at the default step and 25/30/35 px at the large one, so the two
// larger slots each gain a clean 5 px. UiFontLadder::applyTo() adds those pixels to every metric
// that has to hold a line of the text in question.
void applyUiFontScale() {
  const bool large = SETTINGS.uiFontSize == CrossPointSettings::UI_FONT_SIZE_LARGE;
  renderer.replaceFont(SMALL_FONT_ID, large ? ui10FontFamily : smallFontFamily);
  renderer.replaceFont(UI_10_FONT_ID, large ? ui12FontFamily : ui10FontFamily);
  renderer.replaceFont(UI_12_FONT_ID, large ? ui14FontFamily : ui12FontFamily);

  // UiFontLadder::STEPS is what the themes size their rows from, so it has to describe the
  // fonts that were just bound. Regenerating a face can change its advanceY, and a skew here
  // would show up as clipped or floating menu text on every screen with no other symptom.
  const UiFontLadder::Step& step = UiFontLadder::STEPS[large ? 1 : 0];
  const int live[] = {renderer.getLineHeight(SMALL_FONT_ID), renderer.getLineHeight(UI_10_FONT_ID),
                      renderer.getLineHeight(UI_12_FONT_ID)};
  if (live[0] != step.small || live[1] != step.body || live[2] != step.title) {
    LOG_ERR("MAIN", "UI font ladder step %d stale: table %d/%d/%d, fonts %d/%d/%d", large ? 1 : 0, step.small,
            step.body, step.title, live[0], live[1], live[2]);
  }
}

void setupDisplayAndFonts(bool seamless = false, bool skipSdFontDiscovery = false) {
  display.begin(seamless);
  // Carry the panel's measured FAST cost across the reboot. Without this the first decision that
  // depends on it is made blind, and on a slow panel that means one avoidable slow repaint per
  // boot before the measurement catches up. Seed-only: a live measurement always wins.
  display.seedLastFastRefreshMs(SETTINGS.measuredFastRefreshMs);
  renderer.begin();
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  // Bound once and never rebound: applyUiFontScale() leaves these alone, because their whole
  // purpose is to be the size that does not move when the ladder does.
  renderer.insertFont(FIT_SMALL_FONT_ID, smallFontFamily);
  renderer.insertFont(FIT_BODY_FONT_ID, ui10FontFamily);
  renderer.insertFont(FIT_TITLE_FONT_ID, ui12FontFamily);
  renderer.insertFont(BOOKERLY_14_FONT_ID, bookerly14FontFamily);
  renderer.insertFont(BOOKERLY_10_FONT_ID, bookerly10FontFamily);
  renderer.insertFont(BOOKERLY_12_FONT_ID, bookerly12FontFamily);
  renderer.insertFont(BOOKERLY_16_FONT_ID, bookerly16FontFamily);
  renderer.insertFont(BOOKERLY_18_FONT_ID, bookerly18FontFamily);

  renderer.insertFont(NOTOSANS_10_FONT_ID, notosans10FontFamily);
  renderer.insertFont(NOTOSANS_12_FONT_ID, notosans12FontFamily);
  renderer.insertFont(NOTOSANS_14_FONT_ID, notosans14FontFamily);
  renderer.insertFont(NOTOSANS_16_FONT_ID, notosans16FontFamily);
  renderer.insertFont(NOTOSANS_18_FONT_ID, notosans18FontFamily);

  // 20/22/24/26 pt have no faces: they render the 18 pt master scaled, x1.11 to x1.44. Real faces
  // cost about half a megabyte per size (20 pt's eight were 505,656 B) that the app partition does
  // not have. See insertScaledFont() for why the scale belongs to the font ID rather than being
  // threaded through the reader.
  static_assert(CrossPointSettings::BUILTIN_FACE_MAX_POINTS == 18, "the masters below are the 18 pt families");
  for (const auto& rung : CrossPointSettings::FONT_SIZE_RUNGS) {
    if (rung.points <= CrossPointSettings::BUILTIN_FACE_MAX_POINTS) continue;
    const float scale = CrossPointSettings::builtinFaceScale(rung.points);
    renderer.insertScaledFont(CrossPointSettings::getBuiltinReaderFontId(CrossPointSettings::BOOKERLY, rung.size),
                              bookerly18FontFamily, scale);
    renderer.insertScaledFont(CrossPointSettings::getBuiltinReaderFontId(CrossPointSettings::NOTOSANS, rung.size),
                              notosans18FontFamily, scale);
  }
  // The three UI slots are bound by the font ladder rather than registered here, so the first
  // bind and every later rebind run the same code path.
  applyUiFontScale();

  // Discover SD card fonts (under /.crosspoint/fonts/) and load the family
  // currently selected in settings (if any). Safe to call without an SD card.
  // Skipped on the serial-transfer boot path: that activity only uses built-in
  // UI fonts, and it reboots on exit, so Home re-discovers fonts then. Saves an
  // SD directory scan on the reboot the host is waiting through.
  if (!skipSdFontDiscovery) {
    sdFontSystem.begin(renderer);
  }

  LOG_DBG("MAIN", "Fonts setup");
}

// Defined here to satisfy SdCardFontGlobals.h's extern declaration. Keeps
// activity-side callers out of SdCardFontSystem internals.
void ensureSdFontLoaded() { sdFontSystem.ensureLoaded(renderer); }

void unloadSdFontIfLoaded() { sdFontSystem.unload(renderer); }

namespace {
// The cache of the EPUB at `path` when Publisher Fonts apply to it, else "": the setting is on, the book's own style
// is in use, and no font family has been chosen for it, which is how a reader keeps one such book in their own font.
std::string publisherFontsCache(const char* path, const RecentBook& book) {
  const bool embeddedStyle =
      book.embeddedStyleOverride >= 0 ? book.embeddedStyleOverride != 0 : SETTINGS.embeddedStyle != 0;
  if (SETTINGS.publisherFonts == 0 || !embeddedStyle || book.fontFamilyOverride >= 0 ||
      !book.sdFontFamilyOverride.empty()) {
    return {};
  }
  return Epub::cachePathFor(path, "/.crosspoint");
}
}  // namespace

bool bookFontsPendingForPath(const char* path) {
  if (!path || !FsHelpers::hasEpubExtension(std::string_view(path)) || SETTINGS.publisherFonts == 0) return false;
  const std::string cachePath = publisherFontsCache(path, RECENT_BOOKS.getBookByPath(path));
  return !cachePath.empty() && BookFonts::read(cachePath) == BookFonts::State::Pending;
}

void ensureSdFontLoadedForPath(const char* path, const bool waitForBookFonts) {
  if (!path) {
    sdFontSystem.selectBookFamilies({});
    ensureSdFontLoaded();
    return;
  }
  // Writing a not-yet-cached SD font into the flash partition stalls the open for a
  // moment; advertise it. Callers (ReaderActivity::onEnter and the reader) already hold a
  // RenderLock, so the lambda must not take one. Fires only on a genuine first load.
  const auto onColdFontLoad = [] { GUI.drawPopup(renderer, tr(STR_LOADING_FONT)); };

  const std::string_view filePath(path);
  const bool isTxtMd = static_cast<bool (*)(std::string_view)>(FsHelpers::hasTxtExtension)(filePath) ||
                       static_cast<bool (*)(std::string_view)>(FsHelpers::hasMarkdownExtension)(filePath);
  if (isTxtMd) {
    // TXT/MD has no per-book SD font override — use global settings directly.
    sdFontSystem.selectBookFamilies({});
    sdFontSystem.ensureLoaded(renderer, SETTINGS.txtSdFontFamilyName, SETTINGS.txtFontPointSize, onColdFontLoad);
    return;
  }

  // For EPUB: honour per-book SD font and/or size overrides. The book record
  // is available here — the reader activity hasn't started yet, but
  // RecentBooksStore already has the persisted overrides for this path.
  const RecentBook book = RECENT_BOOKS.getBookByPath(path);
  const uint8_t effectiveSize =
      (book.fontSizeOverride >= 0) ? static_cast<uint8_t>(book.fontSizeOverride) : SETTINGS.fontPointSize;

  // Publisher Fonts: not examined yet, what is loaded stays, until the reader has examined the book and asks again.
  const std::string cachePath = publisherFontsCache(path, book);
  if (waitForBookFonts && !cachePath.empty() && BookFonts::read(cachePath) == BookFonts::State::Pending) return;

  // No family override: the global SD font (if any), at the effective size.
  const char* family = SETTINGS.sdFontFamilyName;
  if (!book.sdFontFamilyOverride.empty()) {
    family = book.sdFontFamilyOverride.c_str();  // per-book SD font override
  } else if (book.fontFamilyOverride >= 0) {
    family = "";  // per-book built-in font override: no SD font needed; unload if one was active
  }
  // The families BookFonts copied out of the book: its body family in place of the reader's font, and its accent
  // families beside whichever draws the body. They load together, the body's first and the most used accent next.
  sdFontSystem.selectBookFamilies(cachePath, SETTINGS.publisherFonts, family);
  if (sdFontSystem.bookFamilyName()[0] != '\0') family = sdFontSystem.bookFamilyName();
  const char* families[1 + BookFonts::kMaxAccents] = {family};
  for (uint8_t n = 0; n < BookFonts::kMaxAccents; ++n) families[1 + n] = sdFontSystem.accentFamilyName(n);
  sdFontSystem.ensureLoaded(renderer, families, 1 + BookFonts::kMaxAccents, effectiveSize, onColdFontLoad);
}

// --- Temporary boot-phase heap-corruption bisect probes -------------------------------
// Field data shows the heap can already be corrupt at the existing setup() check even
// on the boot AFTER esp_restart()'s full DRAM re-init — i.e. the corruption is
// re-created during early init, not inherited from the crashed session. These probes
// record WHICH phase breaks it; results are logged once serial is up.
//   preCtors  — C constructor at priority 101, runs before all C++ global ctors
//               (default priority 65535). false here ⇒ IDF/Arduino SDK init.
//   setupEntry — first statement of setup(). false here (with preCtors true) ⇒ one of
//               OUR C++ global constructors.
//   (the existing post-OTA check then isolates the OTA/NVS block)
// Remove once the writer is found.
#ifdef ENABLE_BOOT_HEAP_DIAGNOSTICS
static bool s_heapOkPreCtors = true;
static bool s_heapOkSetupEntry = true;
extern "C" __attribute__((constructor(101))) void heapProbePreCppCtors() {
  s_heapOkPreCtors = heap_caps_check_integrity_all(/*print_errors=*/false);
}
#endif

#ifdef ENABLE_BOOT_HEAP_DIAGNOSTICS
// Last probe of this TU — constructed after every global declared above (slot 5).
static BootHeapProbe s_probeMainLast(5);
#endif

// Maps the user's configured power-button sleep gesture(s) to the matching wake
// gesture(s), so waking mirrors however the device was put to sleep: a long hold
// wakes from a long-hold sleep, a double click wakes from a double-click sleep, and
// so on. Several can be configured at once (e.g. short AND long both power off), in
// which case any one of them wakes it. Relies on SETTINGS already having been loaded
// via loadStartupFromNvs() — btnShortPower/btnDoublePower/btnLongPower are NVS-backed
// specifically so this decision doesn't need the full settings file loaded from SD.
static HalGPIO::WakeGestures wakeGestureFromSettings() {
  using BA = CrossPointSettings::BUTTON_ACTION;
  HalGPIO::WakeGestures gestures;
  gestures.shortAllowed = SETTINGS.btnShortPower == BA::BTN_SLEEP;
  gestures.doubleClick = SETTINGS.btnDoublePower == BA::BTN_SLEEP;
  // Long-press-to-sleep is not user-remappable (loop()'s power-hold timer always owns
  // it, independent of btnLongPower's value — see the "not user-remappable" comment at
  // its call site), so long-hold-to-wake is unconditionally enabled to match.
  gestures.longHold = true;
  return gestures;
}

#ifdef ENABLE_SERIAL_LOG
// Open the USB-CDC log wire when a host is actually on the other end.
//
// This is the ONLY HWCDC::begin() in the firmware. -DARDUINO_USB_MODE=1 compiles
// out the core's CDC-on-boot begin() (cores/esp32/main.cpp guards it with
// !ARDUINO_USB_MODE; HWCDC.cpp only instantiates the object), so skipping this
// call leaves HWCDC with no TX ring buffer and no ISR — nothing can ever set its
// "connected" flag, `if (logSerial)` in logPrintf() stays false, and every LOG_*
// line is dropped for the rest of the session while still filling the RTC ring.
//
// Gated rather than unconditional because begin() allocates the 4 KB RX + 8 KB TX
// buffers below, which are pure waste on a device running from battery.
//
// The gate has to be the enumerated host link, not the charge state. X3 has no
// pin-level USB detect (GPIO20 is I2C SDA there), so its electrical check infers
// a cable from BQ27220 charge current — which reads false on a full battery or a
// data-only cable. That is how an X3 with a serial monitor attached could stay
// mute for a whole session while X4, which reads VBUS off a pin, never did.
// isUsbConnected() now leads with the IDF's SOF-based host-link flag, and the
// call is retried on USB state changes so a cable plugged after boot still gets
// a log.
static bool serialLogOpen = false;

static bool openSerialLogIfHostPresent() {
  if (serialLogOpen) return false;

  // The host link is the signal that matters; the charge inference behind
  // isUsbConnected() is only consulted when there is none, since on X3 it costs
  // an I2C read and can only add a (harmless) false positive here.
  const bool hostLink = gpio.isUsbHostLinkActive();
  // Close the gate only on a REAL negative. A board with no cable detect (X4
  // Pro, T5S3: usbDetect unassigned) has nothing behind isUsbConnected() but the
  // same SOF check hostLink already made, so a false there means "no host
  // enumerated in this instant", not "no cable" — and a host that attaches a
  // moment after boot, or a SOF read that lands in a quiet window, would cost
  // the whole boot log. That is the T5S3 failure again (7c9f6623), where six
  // rounds went into debugging a board that was simply not talking.
  const bool cableDetectable = gpio.canDetectUsbElectrically();
  const bool cable = cableDetectable && gpio.isUsbConnected();
  if (!hostLink && cableDetectable && !cable) {
    // Ring-buffer only — the wire is closed by definition. Surfaces in the crash
    // report's "Last logs", so a session that produced no serial output at all
    // can still be explained after the fact.
    LOG_INF("MAIN", "Serial log not opened: no USB host link, no charge-inferred cable");
    return false;
  }

  // HWCDC's buffer setters delete and recreate the RX queue and TX ring without masking the USB
  // Serial/JTAG interrupt, whose handler uses both (hw_cdc_isr_handler checks tx_ring_buf, then
  // receives from it). With CDC_ON_BOOT the core began Serial before setup(), so on a warm reboot
  // with a host still attached the handler is live and a resize can race it into a freed buffer:
  // after CMD:REBOOT the boot log stopped right here and the device answered nothing until a USB
  // reset (X3, 2026-10-18; reproduced on the first REBOOT of a loop). A cold boot has no host
  // traffic yet, which is why it never showed there. Mask the interrupt for the resize;
  // Serial.begin() below re-enables exactly the interrupts it uses.
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
#endif
  // Enlarge the USB-CDC RX buffer from the 256-byte default before begin().
  // The serial file-transfer protocol receives 2048-byte chunks in bursts; a
  // 256-byte ring drops bytes whenever the byte-by-byte drain stalls briefly,
  // which surfaces as "Timeout waiting for ACK" / failed uploads. 4096 matches
  // MicroReader's usb_serial_jtag rx_buffer_size.
  logSerial.setRxBufferSize(4096);
  // Enlarge the TX buffer too (default 256) so file downloads stream out in
  // larger bursts without the device having to block mid-chunk on a full ring
  // (a full ring that doesn't drain within HWCDC's tx timeout flips the link
  // to "disconnected" and silently drops TX).
  logSerial.setTxBufferSize(8192);
  Serial.begin(115200);
  // Only wait for the host when we have positive evidence one is there. On a
  // board that cannot detect a cable we open the log speculatively, and paying
  // 500 ms on every battery boot for a host that is not coming is not a trade
  // worth making.
  if (hostLink || cable) {
    const unsigned long start = millis();
    while (!Serial && (millis() - start) < 500) {
      delay(10);
    }
  }
  serialLogOpen = true;
  LOG_INF("MAIN", "Serial log opened (%s)",
          hostLink ? "enumerated host link" : (cable ? "charge-inferred cable" : "board has no cable detect"));
  return true;
}
#endif

void setup() {
  // FIRST statement, ahead of the NVS read and the wake gate: on the X4 the battery
  // MOSFET is gated by GPIO13 (BoardProfile power.latch0), and BoardConfig documents a
  // hardware revision in the field that does NOT self-latch — those units stay powered
  // only while the power button physically bridges the rail. holdPowerRails() drives the
  // latch and is the SDK's documented "call it first thing in setup()" contract
  // (BoardConfig.h: selectDevice() re-asserts it once the real board is known, on the
  // premise that the consumer already called it once).
  //
  // Until this landed, the latch was first asserted inside gpio.begin() — after the NVS
  // read, after the wake gate's up-to-300 ms hold window, after three full
  // heap_caps_check_integrity_all() walks and the OTA state read. A non-self-latching
  // unit therefore had to be held for the whole of that (plus the ~200-300 ms bootloader
  // that millis() does not count) or the rail dropped mid-boot and nothing happened at
  // all: the panel still showed the sleep screen, so it read as "stuck asleep, the power
  // button does nothing" (issue #155). Asserting it here is a no-op on self-latching
  // units and costs one GPIO write.
  //
  // Safe before device detection: the dual C3 binary boots with the X4 profile, whose
  // latch0 is GPIO13; the X3 profiles leave latch0 unassigned and carry GPIO13 as their
  // SD rail enable, which wants HIGH at boot anyway. selectDevice() in gpio.begin()
  // re-runs holdPowerRails() with the resolved profile. It stays ahead of the serial
  // bring-up below because that one spends 250 ms in delay(): on a non-self-latching
  // unit the rail must already be held before anything blocks for that long.
  BoardConfig::holdPowerRails();

#ifdef ENABLE_SERIAL_LOG
  // Earliest possible Serial setup, and UNCONDITIONAL — see the isUsbConnected()
  // gate further down, which sizes the transfer buffers but must NOT gate this.
  // A board with no usbDetect pin (T5S3: usbDetect = PIN_UNASSIGNED) can never
  // satisfy that gate from electrical detection, so Serial.begin() never ran
  // there and no log line could reach the host however the transport was set.
  //
  // The 250 ms stall lets the USB Serial/JTAG peripheral finish power-on and the
  // host complete enumeration before we touch CDC state; without it a cold boot
  // races and the board has to be physically replugged before logs flow (a warm
  // reboot hides this, because USB is already enumerated). Ported from
  // upstream/feat-touch, which carries the same reasoning.
  delay(250);
  Serial.begin(115200);
#endif
#if defined(ENABLE_SERIAL_LOG) && defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  // FIRST STATEMENT IN setup(), AND LOAD-BEARING. Do not move it later or raise
  // the value.
  //
  // HWCDC's default TX timeout is 100 ms, and a log write blocks for it whenever
  // no host is draining the endpoint. On a transport that writes unconditionally
  // (FREEINK_LOG_TRANSPORT_USB_CDC_WRITE — the T5S3, where HWCDC's `operator
  // bool` reads false under a monitor, so the readiness guard cannot be used),
  // that is per log line. A boot's worth of them blocks long enough to trip the
  // INTERRUPT watchdog: TG1WDT_SYS_RST before one line reaches the wire.
  //
  // This previously sat ~126 lines into setup(), after gpio.begin() and several
  // BootDiag::markPhase()/LOG_ calls — i.e. after the damage was already done, which is
  // exactly how it looked like a hang rather than a logging problem. It is also
  // deliberately outside the isUsbConnected() gate further down: the T5S3 has no
  // usbDetect pin, so that gate can read false precisely when this matters.
  // CDC_ON_BOOT means the core has already begun Serial, so this is valid here.
  logSerial.setTxTimeoutMs(1);
#endif
  BootDiag::markPhase(BootPhase::SetupEntry);
  // Load just the settings we need before any other init, so the wake gesture mirrors
  // whichever press type(s) the user configured to put the device to sleep.
  SETTINGS.loadStartupFromNvs();
  BootDiag::markPhase(BootPhase::NvsSettings);
  BootDiag::setWakeCheck(
      gpio.verifyPowerButtonWakeup(wakeGestureFromSettings(), CrossPointSettings::getPowerWakeHoldDuration()));
  BootDiag::markPhase(BootPhase::WakeGate);

  // print_errors=true: the corrupt block's address and overwritten values go to the
  // boot console (USB-CDC on boot — the same channel the panic dumps reach), giving the
  // exact damaged block for THIS build so the written value can be symbolized.
#ifdef ENABLE_BOOT_HEAP_DIAGNOSTICS
  s_heapOkSetupEntry = heap_caps_check_integrity_all(/*print_errors=*/true);
#else
  heap_caps_check_integrity_all(/*print_errors=*/true);
#endif
  {
    esp_ota_img_states_t otaState;
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &otaState) == ESP_OK && otaState == ESP_OTA_IMG_PENDING_VERIFY) {
      esp_ota_mark_app_valid_cancel_rollback();
    }
  }

  // Heap integrity check — must run before any allocation.
  //
  // The ESP32-C3 does not zero DRAM on a soft/WDT reset, so the heap metadata
  // from the previous boot is still in place. If that boot ended with a watchdog
  // timeout while malloc/free was mid-flight, the TLSF free-block sentinel
  // (0xabba1234) will have been partially overwritten. Any subsequent heap walk
  // (malloc, free, getMaxAllocHeap, heap_caps_check_integrity) will then read
  // the corrupted sentinel and either crash or loop until the interrupt WDT fires,
  // producing another WDT reset — a self-perpetuating crash loop.
  //
  // Mitigation: detect corruption here and reset immediately via esp_restart().
  // esp_restart() triggers a SW_CPU_RESET which goes through the ROM bootloader,
  // re-initialises DRAM fully, and produces a clean heap on the following boot.
  // A RTC_NOINIT latch prevents infinite loops if the corruption re-appears.
  //
  // print_errors=true writes details to UART0 before Arduino Serial is open.
  {
    static constexpr uint32_t HEAP_CORRUPTION_BOOT_MAGIC = 0xBEEF4321;
    const bool alreadyAttempted = (heapCorruptionBootLatch == HEAP_CORRUPTION_BOOT_MAGIC);
    if (!alreadyAttempted && !heap_caps_check_integrity_all(/*print_errors=*/true)) {
      heapCorruptionBootLatch = HEAP_CORRUPTION_BOOT_MAGIC;
      esp_restart();  // clean DRAM reset — heap will be intact on next boot
    }
    heapCorruptionBootLatch = 0;  // clear latch on a clean-heap boot
  }
  const bool heapIntactAtBoot = heap_caps_check_integrity_all(/*print_errors=*/false);

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Bound the target range too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const uint32_t silentRebootTargetSnapshot =
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_MAX) ? silentRebootTarget : 0;
  // Read-and-clear alongside the magic: a stale 1 here must not outlive the reboot that set it.
  const bool lightWasOnAtSilentReboot = isSilentReboot && silentRebootLightOn == 1;
  silentRebootMagic = 0;
  silentRebootTarget = 0;
  silentRebootLightOn = 0;
  if (!isSilentReboot) {
    heapRecoveryRestartLatch = 0;
  }

  // When rebooting back into the USB serial-transfer activity (after the host's
  // open-triggered reset), keep all boot LOG_* off the wire so the reconnecting
  // host sees a clean binary protocol stream, not interleaved boot logs. The
  // activity unmutes on exit; a plain boot is unaffected.
  if (silentRebootTargetSnapshot == SILENT_REBOOT_TARGET_SERIAL_TRANSFER) {
    setSerialWireMuted(true);
  }

#if defined(FREEINK_DEVICE_LILYGO) && FREEINK_DEVICE_LILYGO
  // LilyGo T5S3 board-support, FIRST: its begin() owns the shared I2C bring-up
  // and the SD SPI bus, deselects the LoRa radio that shares that bus (an
  // undriven CS there corrupts SD transactions), disables LoRa/GPS, configures
  // the BOOT button, and registers the PCA9535 expander's user button through
  // InputManager::setButtonHook(). Without it the board has no working buttons
  // at all and an uncontested SPI bus. Placement matches upstream/feat-touch.
  BoardT5S3::begin();

  // The board-profile corrections that used to live here (capacitive home key,
  // PCF8563 RTC, bezel insets) are now upstream in LILYGO_T5_PRO_GT911 via
  // Free-Ink/freeink-sdk#47, so the overrides are gone. BoardT5S3::begin() above
  // still has to run before gpio.begin(), because it owns the shared I2C bring-up
  // and the SD SPI bus and registers the PCA9535 button hook.
#endif

  powerManager.setSleepStepHook(onSleepStep);
  HalSystem::begin();
  // Create the shared-SPI-bus mutex before anything can touch the panel or the
  // SD card, so no first-use allocation happens inside a refresh or read path.
  HalSpiBus::begin();
  // Same reasoning for the shared I2C bus (touch + RTC + fuel gauge): create the
  // mutex before gpio.begin() can start the sampler, so no first-use allocation
  // lands in a touch poll. Compiles away on non-touch boards.
  HalI2cBus::begin();
#if BOARD_SUPPORT_OWNS_BUSES
  // One lock for one bus. The board-support layer drives the PCA9535 and the
  // TPS65185 under a mutex of its own, and LovyanGFX's EPD panel task runs those
  // helpers on EVERY refresh to raise the panel rails -- while the GT911 is
  // polled from the input sampler and the RTC and gauge from this task, all
  // under HalI2cBus. Two mutexes over one Wire serialise nothing, and the
  // failure is not a bad register read: a collided power-up returns false,
  // Panel_EPD's task ignores that, and the frame is clocked out with the
  // high-voltage rails down -- a page that lands weakly or not at all, then gets
  // repainted by the next refresh.
  //
  // Here rather than inside HalI2cBus because this is the one file that already
  // knows which board it is; the HAL takes a handle and asks no questions.
  HalI2cBus::adoptMutex(BoardT5S3::i2cMutexHandle());
#endif
  gpio.begin();
  // Bus start-up must follow gpio.begin(): that runs inputMgr.begin(), and on a
  // touch board the SDK's GT911 init starts the shared I2C bus itself. The owner
  // detects that and stands down rather than re-initialising it. See HalI2cBus.h.
  HalI2cBus::ensureBusStarted();
  powerManager.begin();
  halTiltSensor.begin();
  gpio_deep_sleep_hold_dis();  // Release deep sleep GPIO hold state from previous sleep cycle
  BootDiag::markPhase(BootPhase::HwInit);

  const auto wakeupReason = gpio.getWakeupReason();

  if (wakeupReason == HalGPIO::WakeupReason::AfterUSBPower) {
#if FREEINK_CAP_USB_MSC
    // Boards that can act as a USB drive must stay awake here. Leaving USB Drive
    // reboots the device with the cable still plugged in, so this wake reason is
    // the normal way back — sleeping through it would strand the reader in a
    // replug loop with no USB Serial/JTAG console to see it happen.
    LOG_DBG("MAIN", "Wakeup reason: After USB Power => staying awake (USB Drive board)");
#else
    // If USB power caused a cold boot, go back to sleep immediately without initializing subsystems
    LOG_DBG("MAIN", "Wakeup reason: After USB Power => Deep sleep");
    halTiltSensor.deepSleep();
    // Same policy the device slept under, not startDeepSleep()'s default — see
    // keepClockAliveForSleep(). saveBeforeSleep() no-ops when the clock was never
    // restored (it returns early on !isSynced()), which is the case this early in boot.
    const bool keepLpAlive = keepClockAliveForSleep();
    // This boot is being abandoned before Storage.begin(), so it writes no boot record,
    // and on an X4 sleeping with the latch cut the breadcrumb dies with the rail. Count it
    // in NVS instead — otherwise a run of these is invisible and the device just looks dead.
    BootDiag::noteAbortedBoot(BootDiag::SleepTrigger::UsbPowerBoot, BootDiag::wakeCheck().verdict);
    BootDiag::beginSleep(BootDiag::SleepTrigger::UsbPowerBoot, keepLpAlive, /*fromReader=*/false);
    HalClock::saveBeforeSleep(keepLpAlive);
    powerManager.startDeepSleep(gpio, keepLpAlive);
    return;
#endif  // FREEINK_CAP_USB_MSC
  }

#ifdef ENABLE_SERIAL_LOG
  if (openSerialLogIfHostPresent()) {
    BootDiag::markPhase(BootPhase::SerialUp);
  }
#endif

  // Same fix as SystemStatus::collectFast(): the old deviceIsX3() ternary logged
  // "X4" on every S3 board, because deviceIsX3() is false there by construction.
  LOG_INF("MAIN", "Hardware detect: %s (%ux%u)", HalCapabilities::boardDisplayName(BoardConfig::ACTIVE.board),
          BoardConfig::ACTIVE.displayWidth, BoardConfig::ACTIVE.displayHeight);
  // The gate ran before Serial was open, so this is the first chance to report it. INF
  // level: a rejected wake leaves no other trace, and release builds must be able to
  // answer "why did nothing happen when I pressed power".
  LOG_INF("BOOT", "Wake gate: %s (decided at %u ms, gate saw the press for %u ms, required %u ms)",
          HalGPIO::wakeVerdictName(BootDiag::wakeCheck().verdict), BootDiag::wakeCheck().decidedAtMs,
          BootDiag::wakeCheck().heldMs, CrossPointSettings::getPowerWakeHoldDuration());
  LOG_DBG("MAIN", "Wakeup reason: %d, millis=%lu, rawPowerPin=%d", static_cast<int>(wakeupReason), millis(),
          digitalRead(InputManager::POWER_BUTTON_PIN) == LOW);
#ifdef ENABLE_BOOT_HEAP_DIAGNOSTICS
  // Re-log the boot-time heap integrity results now that serial is open. The three
  // probes bracket the boot phases: preCtors CORRUPT ⇒ IDF/Arduino SDK init;
  // setupEntry CORRUPT (preCtors OK) ⇒ one of our C++ global constructors;
  // postOta CORRUPT (setupEntry OK) ⇒ the OTA-rollback/NVS block at the top of setup().
  LOG_INF("MEM", "Heap integrity at boot: preCtors=%s setupEntry=%s postOta=%s", s_heapOkPreCtors ? "OK" : "CORRUPT",
          s_heapOkSetupEntry ? "OK" : "CORRUPT", heapIntactAtBoot ? "OK" : "CORRUPT");
  {
    // Static-init bisect pairs (see BootHeapProbe.h). A CORRUPT slot whose paired "pre"
    // slot is OK convicts the global bracketed by that pair; all-OK pairs with
    // setupEntry=CORRUPT mean the writer is a global in an unprobed translation unit.
    const bool* s = bootHeapProbeSlots();
    LOG_INF(
        "MEM",
        "Static-init probes: preDisplay=%s postDisplay=%s preTheme=%s postTheme=%s mainFirst=%s mainLast=%s "
        "preHyphenation=%s postHyphenation=%s preGPIO=%s postGPIO=%s prePower=%s postPower=%s preTilt=%s postTilt=%s",
        s[0] ? "OK" : "CORRUPT", s[1] ? "OK" : "CORRUPT", s[2] ? "OK" : "CORRUPT", s[3] ? "OK" : "CORRUPT",
        s[4] ? "OK" : "CORRUPT", s[5] ? "OK" : "CORRUPT", s[6] ? "OK" : "CORRUPT", s[7] ? "OK" : "CORRUPT",
        s[8] ? "OK" : "CORRUPT", s[9] ? "OK" : "CORRUPT", s[10] ? "OK" : "CORRUPT", s[11] ? "OK" : "CORRUPT",
        s[12] ? "OK" : "CORRUPT", s[13] ? "OK" : "CORRUPT");
  }
  if (!s_heapOkSetupEntry) {
    // Corruption geometry: the bad head sits at SOC_ROM_STACK_START - 0x2000 across
    // builds — exactly 8 KB below the heap's top boundary, which abuts the ROM/startup
    // stack that static init runs on. Dump the words around the canary: a downward
    // stack spill leaves 0x42xxxxxx return addresses and 0x3fcdxxxx frame pointers;
    // anything else points at a different writer.
    const uintptr_t suspect = SOC_ROM_STACK_START - 0x2000;
    const uint32_t* base = reinterpret_cast<const uint32_t*>(suspect - 0x40);
    for (int row = 0; row < 8; row++) {
      LOG_INF("MEM", "dump %08x: %08lx %08lx %08lx %08lx", static_cast<unsigned>(suspect - 0x40 + row * 16),
              static_cast<unsigned long>(base[row * 4]), static_cast<unsigned long>(base[row * 4 + 1]),
              static_cast<unsigned long>(base[row * 4 + 2]), static_cast<unsigned long>(base[row * 4 + 3]));
    }
  }
#endif
  logStartupMemory("after_hw_init");

  if (wakeupReason == HalGPIO::WakeupReason::PowerButton && !BootDiag::wakeCheck().accepted()) {
    LOG_INF("BOOT", "Wake gate rejected the press (%s), returning to deep sleep",
            HalGPIO::wakeVerdictName(BootDiag::wakeCheck().verdict));
    halTiltSensor.deepSleep();
    // Go back down under the SAME policy the device slept under. Rejecting a press is a
    // "nothing happened" answer, so it must not change the power state: with the default
    // keepClockAlive=false this branch cut the X4 battery latch, so a single too-short tap
    // silently downgraded a sleeping device to a powered-off one (see
    // keepClockAliveForSleep()).
    const bool keepLpAlive = keepClockAliveForSleep();
    // Same as the USB path above, and this is the one that matters: a device waking,
    // refusing the press and sleeping again — several times a minute, leaving the panel
    // untouched — is indistinguishable from a dead one unless the aborts are counted
    // somewhere that survives the rail.
    BootDiag::noteAbortedBoot(BootDiag::SleepTrigger::WakeGateRejected, BootDiag::wakeCheck().verdict);
    BootDiag::beginSleep(BootDiag::SleepTrigger::WakeGateRejected, keepLpAlive, /*fromReader=*/false);
    HalClock::saveBeforeSleep(keepLpAlive);
    powerManager.startDeepSleep(gpio, keepLpAlive);
    return;
  }

  // Recovery firmware mode: hold left side button (BTN_UP) together with the power button at
  // boot to skip directly to the SD-card firmware update screen. Useful on devices where USB
  // flashing has been locked down (e.g. recent X3 firmware).
  bool recoveryFirmwareMode = false;
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton) {
    // This window sits between the wake gesture and the splash, so every millisecond here
    // is dead time on a dark screen — it measured 501-508 ms across X3 and X4, roughly
    // 15% of time-to-logo, spent to answer one yes/no question.
    //
    // The combo itself is read from fresh ADC samples (isHeldNow), which need no warm-up.
    // The loop remains only to prime the cache that isPressed() reads, because the "hold
    // Back at boot to skip resuming the reader" check further down consumes that cache and
    // nothing calls update() in between. The 5 ms debounce means a handful of passes is
    // enough; the 500 ms this replaces was not backed by any documented HalGPIO contract.
    const unsigned long primeStart = millis();
    while (millis() - primeStart < 60) {
      gpio.update();
      delay(10);
    }
    // Up normally, Down on a board that wires Up to the MCU's boot-mode strap.
    // There, holding Up through the reset enters ROM download mode instead of
    // running this firmware at all, so the combo could never fire and the user
    // is left looking at a device that appears dead. See upKeyIsBootStrap().
    const bool upIsStrap = HalCapabilities::upKeyIsBootStrap();
    if (gpio.isHeldNow(upIsStrap ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP)) {
      recoveryFirmwareMode = true;
      LOG_INF("MAIN", "Recovery firmware mode (%s + POWER held at boot)", upIsStrap ? "DOWN" : "UP");
    }
    BootDiag::markPhase(BootPhase::RecoverySettle);
  }

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts();
    BootDiag::markPhase(BootPhase::DisplayFonts);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    BootDiag::markPhase(BootPhase::FirstPaint);
    BootDiag::logSummary();
    return;
  }
  BootDiag::markPhase(BootPhase::SdMount);
  // First point at which the card is writable. Files this boot's record and amends the
  // previous sleep's, so the pair "how the last sleep ended" / "how this boot started"
  // is on the card before anything later in setup() can hang or panic.
  BootDiag::persistBoot();
  logStartupMemory("after_storage_begin");

  SETTINGS.loadFromFile();
  // APP_STATE is needed before display init so Quick Resume can skip the on-wake resync
  // and so the seamless-wake path can paint the LoadingIcon over the restored framebuffer.
  APP_STATE.loadFromFile();

  HalSystem::checkPanic();
  HalSystem::clearPanic();  // TODO: move this to an activity when we have one to display the panic info
  // Resolve the setting to a real table index once, here, rather than leaving the "never
  // chosen" sentinel in the field for the rest of the session. activeIndex() is what performs
  // the migration off the retired TIMEZONE enum, and it is idempotent on an index that is
  // already valid -- so doing it on every boot costs nothing and means the picker, the row's
  // displayed value and the next save all see a plain index instead of 255. Runs whether or not
  // a settings file existed, which the load path alone would not cover.
  SETTINGS.clockTimezone = timezones::activeIndex();
  timezones::applyToClock();
  I18N.loadSettings();
  KOREADER_STORE.loadFromFile();
#if CROSSPOINT_KOREADER_AUTOSYNC
  WIFI_STORE.loadFromFile();
  AUTOSYNC_STATE.loadFromFile();
#endif
  OPDS_STORE.loadFromFile();
  WEATHER_SETTINGS.loadFromFile();
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);
  buttonEventManager.setReaderOnTopQuery([] { return activityManager.currentIsReaderActivity(); });

  // Brightness and warmth are always restored. A normal wake starts with the
  // light off unless Restore Light on Wake is enabled; silent maintenance
  // reboots preserve the live state so a heap-recovery restart mid-chapter does
  // not go dark under the reader's hands. Inert on boards without a light.
  // Ported from upstream/develop (crosspoint-reader#2983).
  //
  // The two branches read different sources on purpose. A wake is a fresh start, so it asks the
  // stored PREFERENCE. A silent reboot is the firmware interrupting itself, so it asks what the
  // light was actually DOING when it did -- captured into RTC memory by armSilentReboot().
  // Reading the preference on this path was a bug: it is never cleared when a wake leaves the
  // light off, so "light on, Restore off, sleep, wake, open a WiFi screen" came back lit.
  const bool restoreLightOn =
      isSilentReboot ? lightWasOnAtSilentReboot : (SETTINGS.frontlightOn != 0 && SETTINGS.frontlightRestoreOnWake != 0);
  Frontlight.begin(SETTINGS.frontlightBrightness, SETTINGS.frontlightWarmth, restoreLightOn);
  // After begin(), because a board whose light is probed rather than declared
  // only knows whether it has one by then. Clears mappings this board cannot
  // perform — the case that makes it necessary is an SD card moved from a board
  // with a frontlight to one without, where the stored action would otherwise be
  // both inert and uneditable (the settings screen does not offer it here).
  CrossPointSettings::dropUnsupportedActions(SETTINGS, Frontlight.present(), Frontlight.hasColorTemperature());
  // Navigation follows the screen, not the panel: rotating the device rotates which physical
  // button means "up". The input layer sits below the renderer and so cannot ask it directly —
  // this bridges the two, and is queried live so it can never go stale.
  // See MappedInputManager::buttonFor and issue #87.
  MappedInputManager::setOrientationProvider([] {
    switch (renderer.getHeldOrientation()) {
      case GfxRenderer::Orientation::LandscapeClockwise:
        return MappedInputManager::ScreenOrientation::LandscapeClockwise;
      case GfxRenderer::Orientation::PortraitInverted:
        return MappedInputManager::ScreenOrientation::PortraitInverted;
      case GfxRenderer::Orientation::LandscapeCounterClockwise:
        return MappedInputManager::ScreenOrientation::LandscapeCounterClockwise;
      case GfxRenderer::Orientation::Portrait:
        break;
    }
    return MappedInputManager::ScreenOrientation::Portrait;
  });

  BootDiag::markPhase(BootPhase::ConfigLoad);
  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_DBG("MAIN", "Starting CrossPoint version " CROSSPOINT_VERSION);
  logStartupMemory("before_display_fonts");

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  //
  // Both suppressed-splash flows set showBootScreen=false, so the saved frame is what tells
  // them apart: enterDeepSleep() persists one only for Quick Resume. Present means "restore it
  // and paint the loading icon"; absent means a plain reader resume, which paints nothing at
  // boot so the page gets the X3's one mandatory full sync (see BootResume::ReaderResume).
  const bool haveSleepFrame = !APP_STATE.showBootScreen && Storage.exists(SLEEP_FRAME_FILE);
  const BootResume resume = isSilentReboot             ? BootResume::Silent
                            : APP_STATE.showBootScreen ? BootResume::Splash
                            : haveSleepFrame           ? BootResume::QuickResume
                                                       : BootResume::ReaderResume;

  // Booting straight into the USB serial-transfer activity? Skip SD-font
  // discovery (only built-in UI fonts are used there) to shorten the reboot the
  // host is waiting through. Safe because that activity reboots on exit.
  const bool bootToSerialTransfer = (silentRebootTargetSnapshot == SILENT_REBOOT_TARGET_SERIAL_TRANSFER);
  setupDisplayAndFonts(resume != BootResume::Splash, bootToSerialTransfer);
  BootDiag::markPhase(BootPhase::DisplayFonts);
  logStartupMemory("after_display_fonts");

  switch (resume) {
    case BootResume::Silent:
      // After a silent reboot the panel still shows the previous session's pixels but
      // the SDK's RED-RAM diff buffer was cleared by begin(). A FAST refresh would only
      // flip pixels the SDK *thinks* changed, leaving the old screen visible. Force the
      // first paint to HALF_REFRESH so the panel cleanly repaints; subsequent paints
      // resume FAST as normal.
      renderer.setNextDisplayRefreshMode(HalDisplay::HALF_REFRESH);
      break;
    case BootResume::QuickResume:
      // One-shot flag: re-arm the splash for the next non-quick-resume boot. Save
      // before any painting so a hang in the blocking paint path can't strand
      // us in a quick-resume-with-no-frame loop on the next boot.
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      if (loadSleepFrameBuffer()) {
        // Frame restored: swap the sleep moon for the loading icon. Worth the mandatory full
        // sync here because the restored frame IS the reader page — the user reads their book
        // while it loads rather than watching an interstitial.
        const auto pageHeight = renderer.getScreenHeight();
        renderer.drawImage(LoadingIcon, 0, pageHeight - LOADINGICON_HEIGHT, LOADINGICON_WIDTH, LOADINGICON_HEIGHT);
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      } else {
        activityManager.goToBoot();  // frame file missing, fall back to the splash
      }
      break;
    case BootResume::ReaderResume:
      // Deliberately paints nothing: the panel keeps physically showing the sleep screen until
      // the reader's own first render lands, so the X3's single mandatory full sync is spent on
      // the page instead of an interstitial. Still re-arms the splash for the next plain boot.
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      // Because nothing painted, that first render lands on a panel still holding the sleep image
      // while begin() has just cleared the SDK's RED-RAM diff buffer — and seamless=true above
      // skipped the resync that would otherwise rebase it. A FAST refresh only flips the pixels
      // the SDK thinks changed, so the page composites over the sleep screen instead of replacing
      // it (issue #158). Same failure and the same one-shot cure as the Silent branch above.
      //
      // This does NOT cost the wake an extra refresh: it upgrades the page's OWN paint from FAST
      // to HALF rather than adding an interstitial, so the bargain this branch exists to make is
      // intact. X3 was never affected — Uc8253X3Driver arms _initialFullSyncsRemaining = 1, so its
      // first refresh drives every pixel whatever mode it asks for — and is unchanged by this.
      //
      // The override is renderer-level, so whichever activity paints first consumes it. That also
      // covers the routes where a ReaderResume boot lands on Home instead: Back held at boot,
      // readerActivityLoadCount > 0 after a reader crash, or a quick-resume sleep whose frame file
      // is missing. On the cache-miss path the indexing popup clears it on X4 and re-arms
      // forceHalfRefreshAfterPopup_ for the content page, which is the intended handoff.
      renderer.setNextDisplayRefreshMode(HalDisplay::HALF_REFRESH);
      break;
    case BootResume::Splash:
      activityManager.goToBoot();
      break;
  }
  // The paints above are synchronous (BootActivity::onEnter -> displayBuffer), so this
  // stamp is the moment the panel finished the waveform — i.e. when the user actually
  // sees the logo. On a power-button boot it is also the first visible feedback of any
  // kind, which is what makes the gap back to `gate` the number that matters.
  BootDiag::markPhase(BootPhase::FirstPaint);

  HalClock::restore();
  // Split checkpoints: two boots of the same firmware differed by 14780 B of free heap and, far
  // more importantly, by largest8 65524 vs 26612 at after_activity_route — and the only thing
  // that differed was the reading-stats file (1 book / 110 s vs 36 books / 100788 s). Boot min
  // free was 30044 against 69932, so the load also peaks ~40 KB transiently. That contig gap is
  // what decides whether a big chapter's section build finishes or aborts, so it needs to be
  // attributable to one of these three stores rather than inferred across boots.
  logStartupMemory("before_stores");
  // The recent-books list is no longer loaded here: it is held only while a screen or a call
  // needs it (RecentBooksStore::Hold). Loaded at boot it split the largest free block for the
  // session -- the reader opened every book with 45 KB of it instead of 61 KB (X3).
  logStartupMemory("after_recent_books");
  GLOBAL_BOOKMARKS.load();
  logStartupMemory("after_bookmarks");
  // READING_STATS holds no history: every consumer streams the file (ReadingStatsStore), so there
  // is nothing to load here. Keeping it resident used to cost, measured on X4 with 36 books,
  // ~15 KB of heap and largest8 65524 -> 26612 before the first book was even opened.
  BootDiag::markPhase(BootPhase::StoreLoad);

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (resume == BootResume::Silent && (silentRebootTargetSnapshot == SILENT_REBOOT_TARGET_SLEEP ||
                                              silentRebootTargetSnapshot == SILENT_REBOOT_TARGET_SLEEP_TIMEOUT)) {
    // Sleep was requested while the framebuffers were released (web server
    // session). They exist again now — finish the interrupted sleep transition.
    // The forced HALF_REFRESH from the Silent branch above cleanly repaints the
    // panel (still showing the old session's pixels) with the sleep screen.
    LOG_INF("MAIN", "Resuming sleep transition after framebuffer-recovery reboot");
    enterDeepSleep(/*fromTimeout=*/silentRebootTargetSnapshot == SILENT_REBOOT_TARGET_SLEEP_TIMEOUT);
    return;  // not reached — enterDeepSleep() never returns
  } else if (resume == BootResume::Silent && silentRebootTargetSnapshot == SILENT_REBOOT_TARGET_SERIAL_TRANSFER) {
    // Reset fired while the USB transfer screen was open (host opened the port):
    // land straight back in it so the host's retried command is served.
    activityManager.goToSerialTransfer();
  } else if (resume == BootResume::Silent && silentRebootTargetSnapshot == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
  } else if (resume == BootResume::Silent && silentRebootTargetSnapshot == SILENT_REBOOT_TARGET_CLOCK_SETTINGS) {
    activityManager.goToClockSettings();
  } else if (resume == BootResume::Silent && silentRebootTargetSnapshot == SILENT_REBOOT_TARGET_KOREADER_SETTINGS) {
    activityManager.goToKOReaderSettings();
  } else if (resume == BootResume::Silent && silentRebootTargetSnapshot == SILENT_REBOOT_TARGET_SETTINGS) {
    activityManager.goToSettings();
  } else if (resume == BootResume::Silent && silentRebootTargetSnapshot == SILENT_REBOOT_TARGET_FILE_TRANSFER_OPDS) {
    activityManager.goToFileTransfer(NetworkMode::OPDS_BROWSER);
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    activityManager.goHome();
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    // This is the wake-straight-back-into-the-book branch: tell the wake trace that the open
    // it is about to see is a resume, so its summary line separates resume cost from the cost
    // of an ordinary library open.
    WakeTrace::armResume();
    activityManager.goToReader(path);
  }

  BootDiag::markPhase(BootPhase::ActivityRoute);
  logStartupMemory("after_activity_route");

  // The wake press may still be held. Arm the latch that swallows it (and its release
  // bounce) from the main loop rather than blocking here — see bootPowerReleasePending.
  // Skip on silent reboot: the firmware triggered the restart, so the button isn't held.
  if (!isSilentReboot) {
    bootPowerReleasePending = true;
    bootPowerHighSince = 0;
  }
  // All boot-time power-button handling (which drives inputMgr.update() directly)
  // is done — hand button sampling to the background sampler so presses are caught
  // on a steady ~10ms cadence even while the loop task is busy building sections.
  gpio.startInputSampler();
  SerialTestApi::begin(renderer);
  // Now that the background sampler is live, let long lib-layer tasks (cover image
  // decoders) poll for queued presses and yield so button input keeps priority.
  CooperativeAbort::setLongTaskAbortPredicate(&hasPendingButtonInput);
  // Flush any pin state transitions that occurred during boot before entering the main loop
  mappedInputManager.update();
  buttonEventManager.drain();

  // Block held-power sleep for the first 2 seconds. On Quick Resume the user often releases
  // the wake-press a fraction late; without this guard the loop() power-hold check would
  // immediately fire enterDeepSleep().
  allowSleepAt = millis() + 2000;
  BootDiag::logSummary();
  logStartupMemory("setup_complete");
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.update();
  buttonEventManager.update();

  if (activityManager.requiresExclusiveStorageLoop()) {
    // USB Drive has handed the raw SD card to a host and the filesystem is
    // unmounted. Everything below this point reads settings, writes progress,
    // takes screenshots or navigates — all of which need that filesystem — so
    // run the activity and nothing else. It leaves by rebooting, never by
    // transitioning, so there is no path out of here that we need to service.
    mappedInputManager.update();
    activityManager.loop();
    if (activityManager.preventAutoSleep()) {
      // A host is attached: keep the clock up so MSC callbacks stay responsive.
      powerManager.setPowerSaving(false);
      delay(10);
    } else {
      // Nobody is attached; the activity times the wait out itself rather than
      // sleeping with the card detached, so a slower poll is safe here.
      powerManager.setPowerSaving(true);
      delay(50);
    }
    return;
  }

  // The UI touch gate, refreshed every tick from the setting and the activity on
  // top. The reader keeps touch whatever this says — touchReaderControls governs
  // it there, and giving one behaviour two switches only creates a way for a
  // device to look broken. Reader SUB-screens (contents, dictionary, the menu)
  // are UI screens and are gated with the rest.
  mappedInputManager.setTouchEventsEnabled(SETTINGS.touchUiControls != CrossPointSettings::TOUCH_UI_OFF ||
                                           activityManager.isReaderActivity());
  // Must follow the drain above and precede every power consumer below.
  serviceBootPowerRelease();
  HalClock::updatePeriodic();
  halTiltSensor.update(static_cast<CrossPointTiltPageTurn::Value>(SETTINGS.tiltPageTurn),
                       static_cast<CrossPointOrientation::Value>(SETTINGS.orientation),
                       activityManager.isReaderActivity());
  powerManager.serviceGaugeCapacityLoad();

  renderer.setFadingFix(SETTINGS.fadingFix);
  renderer.setTextDarkness(SETTINGS.textDarkness);
  // Extra clearance from the panel edge, on top of the board profile's own inset. Pushed here
  // rather than once at boot so the Settings screen takes effect on the next render.
  renderer.setViewablePadding(SETTINGS.getEdgeMarginPadding());

  // Re-emit the boot summary when the serial link comes up. A power-up from deep sleep
  // re-enumerates USB, so a monitor attached across the wake misses everything setup()
  // printed — including the trace itself, which is the one line worth having.
  //
  // Strictly bounded, because `Serial` is not a reliable connect signal: HWCDC flips
  // itself to "disconnected" whenever its TX ring doesn't drain within the tx timeout
  // (see the buffer-sizing comment in setup()), which on a busy boot flaps several times
  // a second and once produced eight copies of this summary. The cap keeps a flapping
  // link from spamming the log while still catching a monitor attached late; anything
  // later than that is what `CMD:BOOTLOG` is for.
  {
    // Seeded true because setup() just emitted the summary: if the link was already up
    // then, this must not fire again on the first tick. A battery boot clears it on that
    // same tick (Serial is false), so a cable plugged in later still produces the edge.
    static bool serialWasUp = true;
    static uint8_t bootSummaryRepeats = 0;
    constexpr uint8_t MAX_BOOT_SUMMARY_REPEATS = 2;
    const bool serialIsUp = static_cast<bool>(Serial);
    if (serialIsUp && !serialWasUp && bootSummaryRepeats < MAX_BOOT_SUMMARY_REPEATS) {
      bootSummaryRepeats++;
      BootDiag::logSummary();
    }
    serialWasUp = serialIsUp;
  }

  if (Serial && millis() - lastMemPrint >= 10000) {
    // Keep runtime log lightweight: ESP.getMaxAllocHeap() walks heap metadata
    // and has triggered interrupt WDTs under heavy allocation churn.
    // CPU MHz included so the power-saving state (10 = idle low-power /
    // waveform wait, 160 = normal) is visible in a steady-state log.
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes, CPU: %lu MHz", ESP.getFreeHeap(),
            ESP.getHeapSize(), ESP.getMinFreeHeap(), static_cast<unsigned long>(getCpuFrequencyMhz()));
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
    // Second line, tagged "PSRAM:", so scripts/debugging_monitor.py can tell the
    // two pools apart and give PSRAM its own subplot. The C3 emits nothing here
    // and its graph is unchanged. MaxAlloc is omitted for the same reason as the
    // internal-heap line above -- walking heap metadata on every tick is what
    // triggered the interrupt WDTs.
    LOG_INF("MEM", "PSRAM: Free: %u bytes, Total: %u bytes, Min Free: %u bytes", ESP.getFreePsram(), ESP.getPsramSize(),
            ESP.getMinFreePsram());
#endif
    // Right-sizing aid for the background button sampler task (2 KB allocated).
    // High-water is the min free stack ever seen; shrink the xTaskCreate size if
    // this stays comfortably high across a session.
    LOG_INF("MEM", "btnSampler stack high-water=%u bytes free (min ever)",
            static_cast<unsigned>(gpio.samplerStackHighWater()));
#ifdef ENABLE_BOOT_HEAP_DIAGNOSTICS
    // loop() runs on the Arduino loopTask (8 KB stack), which also drives the reader's
    // sliced background section builds (serviceBackgroundWork → createSectionFile /
    // HTML parse / image dimension reads). That task is NOT covered by the render-task
    // stack instrumentation, yet is an equally plausible source of a stack-into-heap
    // spill (the original crash SP sat against SOC_ROM_STACK_START). High-water is the
    // minimum free stack ever seen by this task (bytes); a small/shrinking value here
    // would point the finger at the loop task rather than the render task.
    LOG_INF("MEM", "loopTask stack high-water=%u bytes free (min ever)",
            static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
#endif
    lastMemPrint = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings.
  // Skip while an activity owns the serial input (e.g. the USB serial
  // file-transfer activity): it drives a binary protocol on logSerial and this
  // line reader would otherwise steal bytes from its stream.
  // Only what has arrived is read; a line still on its way is finished on a later pass
  // (SerialLineBuffer says why). One line per pass, as before: a script's next command waits for
  // the loop to act on this one.
  static SerialLineBuffer serialLine;
  bool serialLineComplete = false;
  if (activityManager.currentOwnsSerialInput()) {
    serialLine.reset();
  } else {
    while (!serialLineComplete && logSerial.available() > 0) {
      serialLineComplete = serialLine.feed(static_cast<char>(logSerial.read()));
    }
  }
  // The serial test commands (CMD:..., see SerialTestApi.h and docs/serial-test-api.md).
  if (serialLineComplete) {
    SerialTestApi::handleLine(serialLine.take());
  }

  // Before the POWER+DOWN block below, which returns while the combo is held.
  ScreenshotUtil::service(renderer, &SerialTestApi::sendScreenshot);

  // Check for any user activity (button press or release, screen touch) or
  // active background work.
  //
  // wasTouchActivity() is the touch analogue of wasAnyPressed/Released and must
  // be here: on a touch board a user can read a whole chapter by tapping and
  // never press a button, and without this the device would sleep under their
  // finger. Always false on non-touch boards.
  static unsigned long lastActivityTime = millis();
  // Start of the auto-sleep countdown. Follows lastActivityTime, and is additionally held
  // while the activity asks to stay awake without counting as activity (Activity::keepAwake()).
  static unsigned long sleepTimerStart = lastActivityTime;
  // isAnyPressed() alongside the edge checks: a button that is DOWN produces no press/release
  // edges, so an edge-only test reads a held finger as an idle device. With IDLE_DOWNCLOCK_MS at
  // 500 ms and ButtonEventManager's long-press at 1000 ms, every long press used to cross the
  // idle threshold mid-hold — the CPU dropped to 10 MHz and the action the long press triggered
  // then ran there. Harmless for most actions, fatal for one: a long press that brings WiFi up
  // (reader -> KOReader sync) reached WiFi.begin() at 10 MHz and wedged. Holding a button is
  // user activity by any reasonable reading, so count it as such. wasTouchActivity() is the same
  // argument for the touch boards: a finger on the glass raises no button edge at all.
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || gpio.isAnyPressed() || gpio.wasTouchActivity() ||
      halTiltSensor.hadActivity() || activityManager.preventAutoSleep()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
    sleepTimerStart = lastActivityTime;
  } else if (activityManager.keepAwake()) {
    // Hold off the sleep timeout only. lastActivityTime also drives the idle governor below, so
    // stamping it here would pin the CPU at full speed for as long as the activity stays awake.
    sleepTimerStart = millis();
  }

  // Power-hold timer for sleep. Hoisted above the screenshot block so the
  // screenshot path can clear it and avoid a stale POWER press triggering sleep
  // after the screenshot completes.
  static unsigned long powerHoldStart = 0;

  static bool screenshotButtonsReleased = true;
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      ScreenshotUtil::request(ScreenshotUtil::Target::Card);
      // Discard the POWER+DOWN presses so they don't fire Short/Long events
      // (e.g. page turn, sleep) once the user releases the combo.
      buttonEventManager.drain();
      powerHoldStart = 0;
    }
    return;
  } else {
    screenshotButtonsReleased = true;
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (millis() - sleepTimerStart >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(/*fromTimeout=*/true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // Track power button hold for sleep.  We require a fresh press edge (wasPressed)
  // before starting to measure hold time, so that a hold carried over from boot
  // (wake-up press) is never misinterpreted as a "go to sleep" press.
  // The power button long-press is not user-remappable, so this path always owns it.
  // Sleep mapped to other buttons is handled by the dispatcher's BTN_SLEEP case below.
  if (gpio.wasPressed(HalGPIO::BTN_POWER)) {
    powerHoldStart = millis();
    LOG_DBG("MAIN", "loop: power button press detected (fresh edge)");
  }
  if (millis() >= allowSleepAt && gpio.isPressed(HalGPIO::BTN_POWER) && powerHoldStart > 0) {
    const unsigned long heldTime = millis() - powerHoldStart;
    if (heldTime > SETTINGS.getPowerButtonDuration()) {
      // If the screenshot combination is potentially being pressed, don't sleep
      if (gpio.isPressed(HalGPIO::BTN_DOWN)) {
        return;
      }
      LOG_DBG("MAIN", "loop: power button held for %lu ms (> %u ms), entering deep sleep", heldTime,
              SETTINGS.getPowerButtonDuration());
      enterDeepSleep();
      // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
      return;
    }
  }

  if (!gpio.isPressed(HalGPIO::BTN_POWER)) {
    powerHoldStart = 0;
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  if (gpio.wasUsbStateChanged()) {
#ifdef ENABLE_SERIAL_LOG
    // A cable (or a host) that arrived after boot: open the log wire now rather
    // than staying mute until the next reboot. No-op once it is open.
    openSerialLogIfHostPresent();
#endif
    activityManager.requestUpdate();
  }

  // Dispatch globally-configured button actions before handing control to the activity.
  // Only non-Default actions are intercepted here; Default falls through to the activity.
  {
    using BA = CrossPointSettings::BUTTON_ACTION;
    using B = MappedInputManager::Button;
    // Five percent, matching CrossInk's frontlight panel. Ten was too coarse:
    // the SDK maps percent to duty through a gamma-1.6554 curve, so most of the
    // usable range sits in the bottom third and a 10-point step there is a large
    // visible jump. The brightness slider remains the way to set an exact level.
    constexpr int LIGHT_STEP_PERCENT = 5;
    auto actionFor = [&](const ButtonEventManager::ButtonEvent& ev) -> uint8_t {
      switch (ev.button) {
        case B::Back:
          switch (ev.type) {
            case ButtonEventManager::PressType::Short:
              return SETTINGS.btnShortBack;
            case ButtonEventManager::PressType::Double:
              return SETTINGS.btnDoubleBack;
            case ButtonEventManager::PressType::Long:
              return SETTINGS.btnLongBack;
          }
          break;
        case B::Confirm:
          switch (ev.type) {
            case ButtonEventManager::PressType::Short:
              return SETTINGS.btnShortConfirm;
            case ButtonEventManager::PressType::Double:
              return SETTINGS.btnDoubleConfirm;
            case ButtonEventManager::PressType::Long:
              return SETTINGS.btnLongConfirm;
          }
          break;
        case B::Left:
          switch (ev.type) {
            case ButtonEventManager::PressType::Short:
              return SETTINGS.btnShortLeft;
            case ButtonEventManager::PressType::Double:
              return SETTINGS.btnDoubleLeft;
            case ButtonEventManager::PressType::Long:
              return SETTINGS.btnLongLeft;
          }
          break;
        case B::Right:
          switch (ev.type) {
            case ButtonEventManager::PressType::Short:
              return SETTINGS.btnShortRight;
            case ButtonEventManager::PressType::Double:
              return SETTINGS.btnDoubleRight;
            case ButtonEventManager::PressType::Long:
              return SETTINGS.btnLongRight;
          }
          break;
        case B::PageBack:
          switch (ev.type) {
            case ButtonEventManager::PressType::Short:
              return SETTINGS.btnShortPageBack;
            case ButtonEventManager::PressType::Double:
              return SETTINGS.btnDoublePageBack;
            case ButtonEventManager::PressType::Long:
              return SETTINGS.btnLongPageBack;
          }
          break;
        case B::PageForward:
          switch (ev.type) {
            case ButtonEventManager::PressType::Short:
              return SETTINGS.btnShortPageForward;
            case ButtonEventManager::PressType::Double:
              return SETTINGS.btnDoublePageForward;
            case ButtonEventManager::PressType::Long:
              return SETTINGS.btnLongPageForward;
          }
          break;
        case B::Power:
          switch (ev.type) {
            case ButtonEventManager::PressType::Short:
              return SETTINGS.btnShortPower;
            case ButtonEventManager::PressType::Double:
              return SETTINGS.btnDoublePower;
            case ButtonEventManager::PressType::Long:
              // Unreachable, and left that way on purpose. The power-hold timer above
              // sleeps the device at getPowerButtonDuration() (400 ms), so the press is
              // long gone before ButtonEventManager's LONG_PRESS_MS (1000 ms) could emit a
              // Long for Power. btnLongPower survives only as the settings screen's way of
              // stating "Long press: Sleep" (SettingsList.h gives it that single option and
              // no selector); dispatching it here would imply it were configurable.
              break;
          }
          break;
        default:
          break;  // Up/Down are the PageBack/PageForward buttons under their list-navigation
                  // names. The FSM emits both names for one press, and the PageBack/PageForward
                  // event above already carries the configured action — so these fall through
                  // to the activity, which is what list-style activities match on.
      }
      return BA::BTN_DEFAULT;
    };
    // Reader-scoped actions are only meaningful inside the reader (they funnel through
    // dispatchButtonAction(), a no-op elsewhere). When such an action is configured on a
    // button but we are NOT in the reader, intercepting it would silently SWALLOW the
    // event — shadowing the current activity's own built-in handling for that button
    // (e.g. RecentBooks long-Left=remove / long-Right=info, which defaulted to
    // BTN_PREV_SECTION / BTN_NEXT_SECTION). Outside the reader we must let the original
    // (button, pressType) event fall through to the activity instead. The predicate is
    // shared with the gesture path, which needs the same answer for the same reason.
    //
    // "In the reader" is currentIsReaderActivity(), the reader ON TOP — the test
    // currentTakesButtonAction() starts from. isReaderActivity() also answers true under a screen opened
    // from the reader (its menu, TOC, starred pages), where the action would be claimed and then
    // dropped: in landscape the printed-page dialog lost its Double (±10) that way, because its
    // digit buttons are the front Left/Right and those carry a reader-scoped double by default.
    //
    // The one exception is a screen that opts in for a button it has no use for of its own: the
    // dictionary definition takes the page turns configured on Power, a button it otherwise ignores.
    const auto isReaderScopedAction = [](const uint8_t a) { return CrossPointSettings::isReaderScopedAction(a); };
    // Executes one resolved action. Extracted from the button loop below so the
    // gesture path runs exactly the same code: a gesture bound to "Reader Menu"
    // must do precisely what a button bound to it does, and two copies of this
    // ladder would not stay that way. Returns false only when the device is on
    // its way into deep sleep and the caller must stop.
    auto runAction = [&](const BA action) -> bool {
      switch (action) {
        case BA::BTN_PAGE_FORWARD:
          activityManager.dispatchButtonAction(BA::BTN_PAGE_FORWARD);
          break;
        case BA::BTN_PAGE_BACK:
          activityManager.dispatchButtonAction(BA::BTN_PAGE_BACK);
          break;
        case BA::BTN_PAGE_FORWARD_10:
          activityManager.dispatchButtonAction(BA::BTN_PAGE_FORWARD_10);
          break;
        case BA::BTN_PAGE_BACK_10:
          activityManager.dispatchButtonAction(BA::BTN_PAGE_BACK_10);
          break;
        case BA::BTN_GO_HOME:
          activityManager.goHome();
          break;
        case BA::BTN_SLEEP:
          enterDeepSleep(/*fromTimeout=*/false, BootDiag::SleepTrigger::ButtonAction);
          // enterDeepSleep() never returns; report it anyway rather than relying
          // on that, so the caller stops processing.
          return false;
        case BA::BTN_FORCE_REFRESH:
        case BA::BTN_FORCE_FAST_REFRESH: {
          const auto mode = action == BA::BTN_FORCE_FAST_REFRESH ? HalDisplay::FAST_REFRESH : HalDisplay::HALF_REFRESH;
          // The activity ON TOP gets the first say: the readers re-render their current page,
          // which keeps its gray planes and never flushes the EPUB reader's pre-rendered next
          // page. This used to ask isReaderActivity(), which is also true under the reader's menu
          // and TOC -- the refresh went to the reader, which was not drawing, and did nothing.
          //
          // Everything else re-flushes the frame on the panel. Synced first: displayBuffer() ends
          // in a buffer swap, so the write buffer holds the frame from TWO refreshes ago, and
          // flushing it as it stood briefly showed the previous screen.
          if (!activityManager.handleForcedRefresh(mode)) {
            RenderLock lock;
            renderer.syncWriteBufferFromDisplayed();
            renderer.displayBuffer(mode);
          }
          break;
        }
        case BA::BTN_OPEN_TOC:
          activityManager.dispatchButtonAction(BA::BTN_OPEN_TOC);
          break;
        case BA::BTN_OPEN_BOOKMARKS:
          activityManager.goToGlobalBookmarks();
          break;
        case BA::BTN_STAR_PAGE:
          activityManager.dispatchButtonAction(BA::BTN_STAR_PAGE);
          break;
        case BA::BTN_FOOTNOTES:
          activityManager.dispatchButtonAction(BA::BTN_FOOTNOTES);
          break;
        case BA::BTN_NEXT_SECTION:
          activityManager.dispatchButtonAction(BA::BTN_NEXT_SECTION);
          break;
        case BA::BTN_PREV_SECTION:
          activityManager.dispatchButtonAction(BA::BTN_PREV_SECTION);
          break;
        case BA::BTN_EXIT_READER:
          activityManager.dispatchButtonAction(BA::BTN_EXIT_READER);
          break;
        case BA::BTN_READER_MENU:
          activityManager.dispatchButtonAction(BA::BTN_READER_MENU);
          break;
        case BA::BTN_KOREADER_SYNC:
          activityManager.dispatchButtonAction(BA::BTN_KOREADER_SYNC);
          break;
        case BA::BTN_TOGGLE_BIONIC_READING:
          activityManager.dispatchButtonAction(BA::BTN_TOGGLE_BIONIC_READING);
          break;
        case BA::BTN_CYCLE_FONT_SIZE:
          activityManager.dispatchButtonAction(BA::BTN_CYCLE_FONT_SIZE);
          break;
        case BA::BTN_FONT_SIZE_SMALLER:
          activityManager.dispatchButtonAction(BA::BTN_FONT_SIZE_SMALLER);
          break;
        case BA::BTN_FONT_SIZE_LARGER:
          activityManager.dispatchButtonAction(BA::BTN_FONT_SIZE_LARGER);
          break;
        case BA::BTN_TOGGLE_TOUCH_UI: {
          const bool enabled = SETTINGS.touchUiControls == CrossPointSettings::TOUCH_UI_OFF;
          SETTINGS.touchUiControls = enabled ? CrossPointSettings::TOUCH_UI_ON : CrossPointSettings::TOUCH_UI_OFF;
          SETTINGS.saveToFile();
          // The gate itself is applied at the top of the next tick, from the
          // setting — one place decides it, so this cannot drift out of step.
          LOG_INF("MAIN", "Touch navigation %s", enabled ? "on" : "off");
          break;
        }
        case BA::BTN_CYCLE_ORIENTATION:
          activityManager.dispatchButtonAction(BA::BTN_CYCLE_ORIENTATION);
          break;
        case BA::BTN_CYCLE_ORIENTATION_BACK:
          activityManager.dispatchButtonAction(BA::BTN_CYCLE_ORIENTATION_BACK);
          break;
        case BA::BTN_QUICK_OVERRIDES:
          activityManager.dispatchButtonAction(BA::BTN_QUICK_OVERRIDES);
          break;
        case BA::BTN_DICTIONARY:
          activityManager.dispatchButtonAction(BA::BTN_DICTIONARY);
          break;
        // Frontlight control is global — handled here rather than through
        // dispatchButtonAction() because it is not the activity's business, and
        // it is as useful on the home screen as it is mid-page. The level is
        // persisted so it survives the next boot; the panel is not redrawn,
        // because nothing on it changed.
        case BA::BTN_LIGHT_TOGGLE:
          Frontlight.setOn(!Frontlight.isOn());
          SETTINGS.frontlightOn = Frontlight.isOn() ? 1 : 0;
          SETTINGS.saveToFile();
          break;
        case BA::BTN_LIGHT_BRIGHTER:
        case BA::BTN_LIGHT_DIMMER: {
          const int delta =
              (static_cast<BA>(action) == BA::BTN_LIGHT_BRIGHTER) ? LIGHT_STEP_PERCENT : -LIGHT_STEP_PERCENT;
          SETTINGS.frontlightBrightness = Frontlight.setBrightnessDelta(delta);
          // A step on a dark panel is meant as "light, this bright", not as a
          // silent no-op the user cannot see.
          if (!Frontlight.isOn()) {
            Frontlight.setOn(true);
            SETTINGS.frontlightOn = 1;
          }
          SETTINGS.saveToFile();
          break;
        }
        case BA::BTN_LIGHT_WARMER:
        case BA::BTN_LIGHT_COOLER: {
          const int delta =
              (static_cast<BA>(action) == BA::BTN_LIGHT_WARMER) ? LIGHT_STEP_PERCENT : -LIGHT_STEP_PERCENT;
          SETTINGS.frontlightWarmth = Frontlight.setWarmthDelta(delta);
          // No "turn the light on" inference here, unlike the brightness steps:
          // changing the colour of a light that is off says nothing about
          // wanting it lit, and it would be a surprising way to blind someone.
          SETTINGS.saveToFile();
          break;
        }
        case BA::BTN_IGNORE:
          // Explicit "do nothing": swallow the event so neither a global action nor the
          // activity's built-in behaviour fires. A press that produced a Long emits no
          // Short, so a long-press mapped to Ignore is fully inert — the release cannot
          // leak through as a page turn.
          break;
        default:
          break;
      }
      return true;
    };

    ButtonEventManager::ButtonEvent ev;
    std::vector<ButtonEventManager::ButtonEvent> defaultEvents;
    defaultEvents.reserve(8);
    while (buttonEventManager.consumeEvent(ev)) {
      const uint8_t action = actionFor(ev);
      // Fall through to the activity when the event has no global effect here: either an
      // explicit Default mapping, or a reader-scoped action while not in the reader -- unless the
      // screen on top takes that action from this button (Activity::takesButtonAction()).
      if (action == BA::BTN_DEFAULT || (isReaderScopedAction(action) && !activityManager.currentTakesButtonAction(
                                                                            static_cast<BA>(action), ev.button))) {
        defaultEvents.push_back(ev);
        continue;
      }
      if (!runAction(static_cast<BA>(action))) return;
    }

    for (auto it = defaultEvents.rbegin(); it != defaultEvents.rend(); ++it) {
      buttonEventManager.pushEventFront(*it);
    }

    // Gestures. In the reader every gesture is live; elsewhere only swipes and
    // two-finger gestures are, because a tap on a list row is that row and no
    // arbitration between the two would be predictable. Swipes are safe to open
    // up: no non-reader screen consumes one, and it is what puts the reading
    // light within reach on the home screen in the dark — the case the reader
    // scope alone left with no answer at all.
    //
    // GestureEventManager reports only gestures the user has actually bound to
    // an action this screen can perform, and suppresses the contact when it
    // reports one. So an unbound gesture is untouched by this, and a bound one
    // cannot both run its action and fall through as a page turn.
    {
      BA gestureAction = BA::BTN_DEFAULT;
      if (gestureEventManager.consumeAction(gestureAction, activityManager.currentIsReaderActivity()) &&
          !runAction(gestureAction)) {
        return;
      }
    }
  }

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

  // After the activity's pass, so a pending CMD:IDLE sees what it queued.
  SerialTestApi::service();
  switch (SerialTestApi::takePowerRequest()) {
    case SerialTestApi::PowerRequest::None:
      break;
    case SerialTestApi::PowerRequest::Sleep:
      // The reply is already out; deep sleep ends the USB link, and only a button wakes the device.
      logSerial.flush();
      enterDeepSleep(/*fromTimeout=*/false, BootDiag::SleepTrigger::SerialCommand);
      return;
    case SerialTestApi::PowerRequest::Reboot:
      logSerial.flush();
      silentRestart();
      return;
  }

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (activityManager.skipLoopDelay()) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    // Two-stage idle backoff, ported from crosspoint-reader PR #2525 (Brian Pugh /
    // @BrianPugh), which measured 9.68 mA -> 2.78 mA idle on an X3 with a PPK2.
    const unsigned long idleMs = millis() - lastActivityTime;
    // A still-held wake press is flushed before it reaches lastActivityTime, so the loop
    // reads as idle while the finger is down. Light sleep arms the power pin as a LEVEL
    // wake, which an already-LOW pin satisfies immediately: the chip would spin through
    // arm/sleep/reject/disarm every tick for as long as the user holds. Poll instead.
    if (idleMs >= HalPowerManager::IDLE_LIGHT_SLEEP_MS && !bootPowerReleasePending) {
      // Idle: light-sleep between polls instead of busy-delaying. Race-to-sleep —
      // run the brief wake windows at the normal clock, not LOW_POWER_FREQ. The
      // board's sleep-floor current is paid per millisecond regardless of CPU
      // speed, so finishing the per-wake work ~16x faster and returning to sleep
      // costs less charge than stretching the window at 10 MHz. The downclock
      // below only serves the pre-sleep 100 Hz polling phase. When lightSleep()
      // declines, the fallback delay() runs at the normal clock too — but that
      // only happens when USB (externally powered), WiFi, or a render Lock (full
      // speed wanted anyway) is active.
      powerManager.setPowerSaving(false);
      if (!powerManager.lightSleep(gpio)) {
        delay(10);  // declined (render Lock, WiFi, USB, mid-debounce): poll at 100 Hz
      }
    } else {
      // Response window after recent input: keep 100 Hz polling for snappy
      // interaction, but downclock once rapid-input bursts have settled — renders
      // re-raise the clock via HalPowerManager::Lock, so full speed only serves
      // loop bookkeeping here.
      if (idleMs >= HalPowerManager::IDLE_DOWNCLOCK_MS) {
        powerManager.setPowerSaving(true);
      }
      delay(10);
    }
  }
}
