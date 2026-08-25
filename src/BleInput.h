#pragma once

// CrossPoint <-> FreeInk BLE HID host glue.
//
// Thin, capability-safe helpers around freeink::BleKeyboardHost (the `BleHid`
// singleton). When FREEINK_CAP_BLE_HID_HOST is compiled out the SDK links stubs,
// so every call here is still valid and simply no-ops / returns false — callers
// need no #ifdefs.
//
// The (kind, value) pair produced by encodeKey() is the stable identity stored in
// CrossPointSettings::bleKeyMap. Page-turner remotes emit "special" keys
// (PageUp/PageDown/arrows); plain keyboards emit usage codes. We deliberately
// ignore modifiers and the printable char for matching (page turners don't use
// modifiers), keeping the persisted entry a trivial two-byte comparison.

#include <BleKeyboardHost.h>

#include <cstdint>

class GfxRenderer;
class MappedInputManager;

namespace bleinput {

// Advertised central name shown to peripherals during pairing.
inline constexpr const char* kHostName = "CrossPoint";

// Heap floor for starting the NimBLE stack (measured begin() cost: ~52-57 KB).
// This must leave the reader usable AFTER the stack is resident, not merely fit the
// stack itself. Framebuffer lending covers a section build's one big inflate window,
// but NOT its storm of small allocations (CSS/sscanf/strings) which draw from the
// general heap; at 56 KB the stack started with ~2 KB general heap left and the next
// build abort()ed inside sscanf (We_baseline from a cleared cache, BLE connected).
// 80 KB is reachable at steady-state reading (~84 KB idle with an SD font + section)
// and leaves ~26 KB general heap after startup; the build pre-flight
// (BUILD_MIN_FREE_HEAP) frees BLE and retries when a build still needs more, and the
// render shed (RENDER_MIN_FREE_HEAP) backstops tight renders.
inline constexpr size_t kStartMinFreeHeap = 80 * 1024;

// Lower floor for the Bluetooth settings screen, where the user has explicitly asked
// for BLE right now (scanning/pairing is dead without the stack). No page renders or
// section builds run there, so the reader-sized reserve above doesn't apply — only
// NimBLE's own ~57 KB plus working margin.
inline constexpr size_t kStartMinFreeHeapExplicit = 70 * 1024;

// Start the BLE HID host (idempotent). Returns false if BLE is compiled out or
// NimBLE init failed. Safe to call repeatedly.
bool ensureStarted();
bool startInProgress();

// Drop the active link (e.g. before deep sleep or when the user disables BT).
void stop();

// Encode a decoded key event into the stable (kind, value) identity used by the
// settings map. kind: 0 = SpecialKey, 1 = HID usage. Returns false when the event
// carries no usable identity (no special key and no usage code).
bool encodeKey(const freeink::KeyEvent& ev, uint8_t& kind, uint8_t& value);

// Human-readable name for a stored (kind, value) identity, for the mapping UI.
// Writes a null-terminated string into out (e.g. "Page Down", "Key 0x4B").
void describeKey(uint8_t kind, uint8_t value, char* out, size_t outLen);

// Draw a "BT Connecting..." popup and pump the BLE host until the bonded remote
// links, the user presses a button to dismiss, or a timeout. No-op if BLE isn't
// running or is already connected. The caller must redraw afterward to clear it.
void showConnectingUntilLinked(const GfxRenderer& renderer, const MappedInputManager& input);

}  // namespace bleinput
