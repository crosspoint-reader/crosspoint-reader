#include <cassert>
#include <cstdio>

#include "UsbDfuService.cpp"

namespace {
std::atomic<bool> testReady{false}, beginRequested{false}, abortRequested{false}, finishRequested{false};
std::atomic<unsigned> prepared{0};
std::atomic<bool> testConfigured{false}, configureRequested{false};
void (*stopHook)() = nullptr;
unsigned rearmed = 0;
}  // namespace
void HalUsbDfu::setConfigured(bool value) { testConfigured.store(value); }
void HalUsbDfu::setReady(bool value) { testReady.store(value); }
void HalUsbDfu::setStopHook(void (*hook)()) { stopHook = hook; }
void HalUsbDfu::arm() { ++rearmed; }
namespace usb_dfu {
bool run(Progress progress, void* context, Prepare prepare, bool (*cancelled)(), bool background,
         void (*linkState)(bool)) {
  assert(background);
  while (!cancelled()) {
    if (configureRequested.exchange(false)) linkState(true);
    if (beginRequested.exchange(false)) {
      const bool accepted = prepare(context);
      prepared++;
      if (accepted)
        progress(Stage::Receiving, 4096, 100000, context);
      else
        progress(Stage::Waiting, 0, 0, context);
    }
    if (abortRequested.exchange(false)) progress(Stage::Waiting, 0, 0, context);
    if (finishRequested.exchange(false)) {
      progress(Stage::Verifying, 100000, 100000, context);
      return true;
    }
    delay(1);
  }
  return false;
}
}  // namespace usb_dfu
template <class F>
void waitFor(F condition) {
  const auto start = millis();
  while (!condition()) {
    assert(millis() - start < 2000);
    delay(1);
  }
}
int main() {
  using namespace usb_dfu;
  failAllocation = true;
  assert(!startBackground() && !snapshot().running && !testReady.load() && !stopHook);
  failAllocation = false;
  assert(startBackground() && testReady.load() && stopHook);
  delay(20);
  assert(!testConfigured.load());  // A task/pullup/charging source is not enumeration.
  configureRequested = true;
  waitFor([] { return testConfigured.load(); });
  assert(snapshot().stage == Stage::Waiting && !takeScreenRequest() && activityManager.paints == 0);
  beginRequested = true;
  waitFor([] { return snapshot().screenRequested; });
  assert(takeScreenRequest() && !takeScreenRequest() && prepared == 0);
  allowTransfer();
  waitFor([] { return snapshot().bytes == 4096; });
  assert(activityManager.paints == 0);
  assert(snapshot().bytes == 4096 && snapshot().stage == Stage::Receiving);
  abortRequested = true;
  waitFor([] { return snapshot().stage == Stage::Waiting; });
  assert(snapshot().running && !snapshot().screenRequested);
  releaseScreen();
  beginRequested = true;
  waitFor([] { return snapshot().screenRequested; });
  rejectTransfer();
  waitFor([] { return prepared == 2 && snapshot().stage == Stage::Waiting; });
  assert(snapshot().stage == Stage::Waiting && activityManager.paints == 0);
  beginRequested = true;
  waitFor([] { return snapshot().screenRequested; });
  stopHook();  // Sleep while UI preparation is pending must release the worker.
  taskThread.join();
  assert(!snapshot().running && !testReady.load() && !testConfigured.load());
  releaseScreen();
  acknowledgeStopped();
  assert(!stopHook && rearmed == 1);
  assert(startBackground());
  beginRequested = true;
  waitFor([] { return snapshot().screenRequested; });
  allowTransfer();
  waitFor([] { return snapshot().bytes == 4096; });
  assert(activityManager.paints == 0);
  finishRequested = true;
  waitFor([] { return !snapshot().running; });
  taskThread.join();
  assert(snapshot().installed && !testReady.load() && activityManager.paints == 1);
  puts(
      "PASS: background connection has no screen request; valid-header handoff, rejection, abort/retry, sleep "
      "shutdown, allocation failure, completion.");
}
