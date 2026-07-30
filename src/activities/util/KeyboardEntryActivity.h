#pragma once
#include <FreeInkUIGfxRenderer.h>
#include <GfxRenderer.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

enum class InputType { Text, Password, Url };

// Text entry on the FreeInkUI keyboard component: the SDK layout tables and
// keyboard() do the key rendering and hit-rect registration, InteractionBuffer
// routes taps/long-presses, and this activity owns the text field, cursor
// editing, and the URL snippet layouts.
class KeyboardEntryActivity : public Activity {
 public:
  explicit KeyboardEntryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 std::string title = "Enter Text", std::string initialText = "",
                                 const size_t maxLength = 0, InputType inputType = InputType::Text)
      : Activity("KeyboardEntry", renderer, mappedInput),
        title(std::move(title)),
        text(std::move(initialText)),
        maxLength(maxLength),
        inputType(inputType) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string title;
  std::string text;
  size_t maxLength;
  InputType inputType;
  bool passwordVisible = false;

  ButtonNavigator buttonNavigator;

  // Keyboard layers. The letter/symbol layers come from the SDK's builtin
  // layouts (with the always-visible number row); the URL layers are
  // app-defined tables in the .cpp.
  freeink::ui::KeyboardLayoutId layoutId = freeink::ui::KeyboardLayoutId::QwertyEn;
  bool shifted = false;
  bool symbols = false;
  bool urlPanel = false;  // URL snippet panel replaces the letter layer

  // Key hit rects registered by the keyboard component during render();
  // loop() routes touch snapshots against them. 5-row EN layout registers 41
  // keys, so 48 leaves headroom.
  freeink::ui::InteractionBuffer<48> interactions;

  // GPIO selection over the current layout grid (row/col in layout terms;
  // the bottom action row is just the last row).
  int selRow = 0;
  int selCol = 0;

  bool confirmHeld = false;
  bool confirmLongHandled = false;

  bool cursorMode = false;
  bool togglePos = false;
  size_t cursorPos = 0;  // byte offset into text (always on a code point boundary)
  bool upHeld = false;
  bool upLongHandled = false;
  bool downHeld = false;
  bool downLongHandled = false;
  bool rightHeld = false;
  bool rightLongHandled = false;
  size_t savedCursorPos = 0;
  size_t rightStartCursorPos = 0;

  // Tap/hold routing (threshold long-press, release swallow, slide re-arm)
  // lives in the SDK; loop() feeds it the level-triggered touch state.
  freeink::ui::TouchHoldRouter touchRouter;

  // loop() runs on the main task while render() rebuilds the interaction
  // table on the render task; routing against a half-built table would read
  // torn entries, so taps are dropped during the rebuild window. atomic (not
  // volatile) so the flag also orders the table writes on dual-core targets.
  std::atomic<bool> interactionsReady{false};

  int delPressCount = 0;
  bool hintVisible = false;
  unsigned long hintShowTime = 0;

  void onComplete(std::string text);
  void onCancel();
  bool cursorPositionFromPoint(int x, int y, size_t& position) const;
  std::string displayTextForCurrentState() const;
  // Advance of s[start, end) measured in place by temporarily null-terminating
  // at `end` — avoids a substr temporary per measurement.
  int measureRange(std::string& s, int start, int end) const;
  // Largest line end in (start, s.length()] whose advance fits maxWidth.
  // Binary search over the monotonic prefix advance; always advances at least
  // one byte so an oversized glyph cannot stall the wrap loop.
  int lineBreakEnd(std::string& s, int start, int maxWidth) const;

  const freeink::ui::KeyboardLayout& currentLayout() const;
  const freeink::ui::KeyboardKey* selectedKey() const;
  int selectedLogicalIndex() const;
  void clampSelection();
  void moveSelectionRow(int delta);
  void moveSelectionCol(int delta);
  bool syncSelectionToValue(int16_t value);
  // Handles one key activation (by stable key id). Returns true when the
  // screen needs a repaint; OK/cancel finish the activity instead.
  bool activateValue(int16_t value, bool longPress);
  bool clearAllOrAltOnSelected();

  void insertUtf8(const char* out);
  bool backspaceUtf8();
  static size_t utf8Prev(const std::string& s, size_t pos);
  static size_t utf8Next(const std::string& s, size_t pos);

  freeink::ui::Rect keyboardRect() const;

  static constexpr uint16_t LONG_PRESS_MS = 500;
  static constexpr uint16_t DEL_LONG_PRESS_MS = 1500;
  static constexpr uint16_t TOUCH_LONG_PRESS_MS = 350;
  static constexpr uint16_t TOUCH_DEL_LONG_PRESS_MS = 900;

  // App-specific key id: toggles the URL snippet panel (URL fields only).
  static constexpr int16_t URL_PANEL_KEY = -3;
};
