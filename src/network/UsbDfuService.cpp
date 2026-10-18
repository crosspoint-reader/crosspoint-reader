#include <sdkconfig.h>
#if CONFIG_IDF_TARGET_ESP32C3 || CROSSPOINT_NATIVE_DFU
#include <Arduino.h>
#include <HalPowerManager.h>
#include <HalUsbDfu.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>

#include "UsbDfuService.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"

namespace usb_dfu {
namespace {
std::atomic<bool> running{false}, installed{false}, stopping{false}, screenRequested{false}, screenReady{false},
    rejected{false};
std::atomic<bool> screenAttached{false};
std::atomic<Stage> stage{Stage::Waiting};
std::atomic<uint32_t> received{0}, total{0}, beganAt{0};

bool cancelled() { return stopping.load(); }
bool prepare(void*) {
  rejected.store(false);
  screenReady.store(false);
  received.store(0);
  total.store(0);
  beganAt.store(millis());
  stage.store(Stage::Receiving);
  screenRequested.store(true);
  const uint32_t start = millis();
  while (!screenReady.load() && !rejected.load() && !stopping.load() && uint32_t(millis() - start) < 6500) delay(1);
  screenRequested.store(false);
  if (screenReady.load() && !stopping.load()) return true;
  stage.store(Stage::Waiting);
  return false;
}
void progress(Stage next, size_t count, size_t size, void*) {
  received.store(count);
  total.store(size);
  stage.store(next);
  if (next == Stage::Waiting) {
    screenReady.store(false);
    screenRequested.store(false);
    // Finish leaving the old screen before another host header can arrive.
    while (screenAttached.load() && !stopping.load()) delay(1);
    return;
  }
  // onEnter paints Receiving once, before allowing the first data block.
  // Keep that frame unchanged until reception ends.
  if (next != Stage::Receiving && screenReady.load()) activityManager.requestUpdateAndWait();
}
void worker(void*) {
  installed.store(run(progress, nullptr, prepare, cancelled, true, HalUsbDfu::setConfigured));
  HalUsbDfu::setConfigured(false);
  HalUsbDfu::setReady(false);
  screenRequested.store(false);
  running.store(false);
  vTaskDelete(nullptr);
}
}  // namespace

bool startBackground() {
  if (running.load()) return true;
  stopping.store(false);
  installed.store(false);
  screenRequested.store(false);
  screenReady.store(false);
  stage.store(Stage::Waiting);
  received.store(0);
  total.store(0);
  beganAt.store(0);
  running.store(true);
  HalUsbDfu::setStopHook(stopBackground);
  HalUsbDfu::setConfigured(false);
  HalUsbDfu::setReady(true);
  powerManager.setPowerSaving(false);
  // USB needs its own stack while book rendering continues. Allocated only
  // for a detected host, then reclaimed by FreeRTOS when the host leaves.
  if (xTaskCreate(worker, "X3Dfu", 8192, nullptr, 2, nullptr) != pdPASS) {
    running.store(false);
    HalUsbDfu::setStopHook(nullptr);
    HalUsbDfu::setConfigured(false);
    HalUsbDfu::setReady(false);
    LOG_ERR("DFU", "Unable to allocate background USB task");
    return false;
  }
  return true;
}
void stopBackground() {
  stopping.store(true);
  while (running.load()) delay(1);
}
Snapshot snapshot() {
  Snapshot s;
  s.stage = stage.load();
  s.bytes = received.load();
  s.total = total.load();
  const uint32_t start = beganAt.load();
  s.elapsedMs = start ? uint32_t(millis() - start) : 0;
  s.running = running.load();
  s.installed = installed.load();
  s.screenRequested = screenRequested.load();
  return s;
}
bool takeScreenRequest() { return screenRequested.exchange(false); }
void allowTransfer() {
  screenAttached.store(true);
  screenReady.store(true);
}
void releaseScreen() { screenAttached.store(false); }
void rejectTransfer() { rejected.store(true); }
void acknowledgeStopped() {
  HalUsbDfu::setStopHook(nullptr);
  HalUsbDfu::arm();
}
}  // namespace usb_dfu
#endif
