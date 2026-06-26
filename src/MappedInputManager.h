#pragma once

#include <HalGPIO.h>

class GfxRenderer;

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward, NavNext, NavPrevious };
  // Number of values in Button (Back..NavPrevious). Used to size the BLE overlay and
  // to clamp persisted BLE mappings. Keep in sync with the enum above.
  static constexpr uint8_t kButtonCount = 11;

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  MappedInputManager(HalGPIO& gpio, const GfxRenderer& renderer) : gpio(gpio), renderer(renderer) {}

  void update() const { gpio.update(); }
  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  bool isPressed(Button button) const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;

  // --- BLE page-turner overlay -------------------------------------------------
  // Drain decoded key events from the FreeInk BLE HID host and translate the ones
  // bound in SETTINGS.bleKeyMap into per-frame logical-button edges that OR into
  // wasPressed()/isPressed()/wasReleased(). Call once per main-loop iteration,
  // right after gpio.update() and BleHid.poll(). No-ops when BLE is compiled out.
  void pollBle();
  // True when a mapped BLE key produced an edge this frame — keeps the inactivity
  // / auto-sleep timer alive while a remote is the only input device in use.
  bool bleHadActivityThisFrame() const { return bleActivityThisFrame; }
  // Capture mode: while on, pollBle() stops mapping events and instead stashes the
  // raw decoded key identity so the button-mapping UI can read it without racing the
  // live mapping over the single popKey() queue.
  void setBleCaptureMode(bool on);
  // Pop a captured (kind, value) key identity grabbed while in capture mode.
  // Returns false when nothing has been captured since the last call.
  bool takeCapturedBleKey(uint8_t& kind, uint8_t& value);

  // True when the control axis is flipped relative to the physical buttons: the user opted into
  // orientation-following front buttons AND the screen is *currently rendered* rotated (INVERTED /
  // LANDSCAPE_CCW). Keyed on the live renderer orientation rather than the persisted reader setting,
  // so portrait UI (home, settings) never swaps while the reader and its menus do.
  [[nodiscard]] bool isNavDirectionSwapped() const;

 private:
  HalGPIO& gpio;
  // Logical-to-physical button mapping depends on what the user is actually looking at: when the
  // screen is rendered rotated, the directional buttons must flip to match. The renderer is the only
  // authority on the *live* orientation (the reader rotates it and restores portrait on exit), so we
  // read it here instead of CrossPointSettings.orientation, which is just the persisted reader
  // preference and stays "rotated" even while portrait UI like home/settings is on screen.
  const GfxRenderer& renderer;

  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
  // OR-in the BLE overlay for a logical button, mirroring mapButton()'s composite
  // handling of NavNext/NavPrevious so a remote key bound to Up/Down/Left/Right also
  // drives list navigation.
  bool bleEdge(const bool* arr, Button button) const;

  // Per-frame BLE overlay, indexed by (uint8_t)Button.
  bool blePressEdge[kButtonCount] = {};    // press edge this frame   -> wasPressed / isPressed
  bool bleReleaseEdge[kButtonCount] = {};  // release edge this frame  -> wasReleased
  bool bleActivityThisFrame = false;
  bool bleCaptureMode = false;
  bool bleHasCaptured = false;
  uint8_t bleCapturedKind = 0xFF;
  uint8_t bleCapturedValue = 0;
};
