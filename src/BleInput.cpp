#include "BleInput.h"

#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <I18n.h>

#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace bleinput {

namespace {
volatile bool g_startInProgress = false;
}

// NimBLE controller init/deinit hang (interrupt WDT) if run at the 10 MHz low-power
// frequency, so force normal CPU speed around both. Centralized here so every caller
// (boot restore, settings toggle, reader toggle, sleep) is covered automatically.
bool ensureStarted() {
  g_startInProgress = true;
  HalPowerManager::Lock powerLock;
  const bool ok = BleHid.begin(kHostName);
  g_startInProgress = false;
  return ok;
}

bool startInProgress() { return g_startInProgress; }

// Full teardown (NimBLE deinit), not just a link drop, so the BLE stack's RAM is
// returned to the heap — otherwise memory-hungry work like EPUB inflate can't
// allocate even after the user turns Bluetooth off.
void stop() {
  HalPowerManager::Lock powerLock;
  BleHid.end();
}

bool encodeKey(const freeink::KeyEvent& ev, uint8_t& kind, uint8_t& value) {
  if (ev.special != freeink::SpecialKey::None) {
    kind = 0;
    value = static_cast<uint8_t>(ev.special);
    return true;
  }
  if (ev.keycode != 0) {
    kind = 1;
    value = ev.keycode;
    return true;
  }
  return false;
}

namespace {
const char* specialName(uint8_t value) {
  switch (static_cast<freeink::SpecialKey>(value)) {
    case freeink::SpecialKey::Enter:
      return "Enter";
    case freeink::SpecialKey::Backspace:
      return "Backspace";
    case freeink::SpecialKey::Tab:
      return "Tab";
    case freeink::SpecialKey::Escape:
      return "Escape";
    case freeink::SpecialKey::Delete:
      return "Delete";
    case freeink::SpecialKey::Left:
      return "Left";
    case freeink::SpecialKey::Right:
      return "Right";
    case freeink::SpecialKey::Up:
      return "Up";
    case freeink::SpecialKey::Down:
      return "Down";
    case freeink::SpecialKey::Home:
      return "Home";
    case freeink::SpecialKey::End:
      return "End";
    case freeink::SpecialKey::PageUp:
      return "Page Up";
    case freeink::SpecialKey::PageDown:
      return "Page Down";
    default:
      return nullptr;
  }
}
}  // namespace

void showConnectingUntilLinked(const GfxRenderer& renderer, const MappedInputManager& input) {
  if (!BleHid.isRunning() || BleHid.isConnected()) return;
  // drawPopup refreshes the panel itself, so draw once and let e-ink hold it while we
  // pump the host. Holds until the remote links, the user presses a button to bail, or
  // a generous timeout (a remote that slept after a disconnect needs a button to wake).
  GUI.drawPopup(renderer, tr(STR_BT_CONNECTING_POPUP));
  const unsigned long deadline = millis() + 10000;
  while (!BleHid.isConnected() && millis() < deadline) {
    BleHid.poll();
    input.update();
    if (input.wasAnyPressed()) break;
    delay(50);
  }
  // Note: the caller must redraw to clear the popup. For grayscale reader pages the
  // caller should also request a ghost-cleanup (HALF) refresh first — a plain fast/
  // partial refresh ghosts badly over the BW popup (see Activity::requestGhostCleanup).
}

void describeKey(uint8_t kind, uint8_t value, char* out, size_t outLen) {
  if (!out || outLen == 0) return;
  if (kind == 0) {
    const char* name = specialName(value);
    if (name) {
      strncpy(out, name, outLen - 1);
      out[outLen - 1] = '\0';
      return;
    }
  }
  // Printable ASCII usage handled as a generic key code; show the raw value.
  snprintf(out, outLen, "Key 0x%02X", static_cast<unsigned>(value));
}

}  // namespace bleinput
