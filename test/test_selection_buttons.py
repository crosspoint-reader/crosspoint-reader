"""Run with python3 test/test_selection_buttons.py (requires a host C++ compiler)."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/activities/reader/ClipSelectionActivity.cpp").read_text()
header = (ROOT / "src/activities/reader/ClipSelectionActivity.h").read_text()
# Compile the production poller with a controllable render lock and button clock.
shared = (ROOT / "src/util/WordSelectionInput.h").read_text()
helpers = "class WordSelectionInput" + shared.split("class WordSelectionInput", 1)[1] + "\nusing Input = WordSelectionInput;\n"
method = "void ClipSelectionActivity::loopButtons()" + source.split(
    "void ClipSelectionActivity::loopButtons()", 1
)[1].split("void ClipSelectionActivity::loop()", 1)[0]
members = "std::array<uint8_t, 16> pendingButtons" + header.split(
    "std::array<uint8_t, 16> pendingButtons", 1
)[1].split("  int fontId", 1)[0]
harness = r"""
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
uint32_t clockMs = 0;
uint32_t millis() { return clockMs; }
int overflows = 0;
#define LOG_ERR(...) (++overflows)
struct RenderLock {
  enum class Mode { Try };
  static inline bool busy = true;
  explicit RenderLock(Mode) {}
  bool ownsLock() const { return !busy; }
};
struct MappedInputManager {
  enum class Button { ScreenLeft, ScreenRight, ScreenUp, ScreenDown, Confirm, Back, NavPrevious, NavNext };
  uint8_t pressed = 0, released = 0, held = 0;
  bool wasPressed(Button b) const { return pressed & (1 << static_cast<int>(b)); }
  bool wasReleased(Button b) const { return released & (1 << static_cast<int>(b)); }
  bool isPressed(Button b) const { return held & (1 << static_cast<int>(b)); }
};
""" + helpers + r"""
struct ClipSelectionActivity {
  MappedInputManager mappedInput;
  int selected = 5, confirmed = -1;
  bool handleButtons(uint8_t buttons) {
    if (buttons & Input::INPUT_CONFIRM) { confirmed = selected; return true; }
    selected += bool(buttons & Input::INPUT_RIGHT) - bool(buttons & Input::INPUT_LEFT);
    return false;
  }
  void loopButtons();
""" + members + "};\n" + method + r"""
int main() {
  ClipSelectionActivity activity;
  auto poll = [&](uint32_t time, uint8_t pressed, uint8_t held, uint8_t released = 0) {
    clockMs = time;
    activity.mappedInput = {pressed, released, held};
    activity.loopButtons();
  };
  // Two quick taps and Confirm arrive while refresh owns the lock.
  poll(0, Input::INPUT_RIGHT, Input::INPUT_RIGHT);
  poll(40, 0, 0);
  poll(90, Input::INPUT_RIGHT, Input::INPUT_RIGHT);
  poll(130, 0, 0);
  poll(180, 0, 0, Input::INPUT_CONFIRM);
  poll(220, Input::INPUT_LEFT, Input::INPUT_LEFT);
  poll(250, 0, 0);
  assert(activity.selected == 5 && activity.pendingButtonCount == 4);
  RenderLock::busy = false;
  poll(700, 0, 0);
  assert(activity.selected == 7 && activity.confirmed == 7);
  assert(activity.pendingButtonCount == 1);
  poll(710, 0, 0);
  assert(activity.selected == 6 && activity.pendingButtonCount == 0);
  // Hold repeats are captured during refresh, then stop upon release.
  RenderLock::busy = true;
  poll(1000, Input::INPUT_RIGHT, Input::INPUT_RIGHT);
  poll(1499, 0, Input::INPUT_RIGHT);
  assert(activity.pendingButtonCount == 1);
  poll(1500, 0, Input::INPUT_RIGHT);
  poll(2000, 0, Input::INPUT_RIGHT);
  poll(2010, 0, 0);
  RenderLock::busy = false;
  poll(3000, 0, 0);
  assert(activity.selected == 9 && activity.pendingButtonCount == 0);
  // A new press resets the repeat delay; unsigned elapsed time survives wrap.
  poll(UINT32_MAX - 100, Input::INPUT_LEFT, Input::INPUT_LEFT);
  poll(398, 0, Input::INPUT_LEFT);
  assert(activity.selected == 8);
  poll(399, 0, Input::INPUT_LEFT);
  assert(activity.selected == 7);
  poll(410, 0, 0);
  // Overflow preserves the queued prefix and reports failure instead of overwriting it.
  RenderLock::busy = true;
  for (int i = 0; i < 17; ++i) poll(500 + i, Input::INPUT_RIGHT, 0);
  assert(activity.pendingButtonCount == 16 && overflows == 1);
  RenderLock::busy = false;
  poll(600, 0, 0);
  assert(activity.selected == 23 && activity.pendingButtonCount == 0);
}
"""
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / "selection.cpp"
    executable = Path(directory) / "selection"
    cpp.write_text(harness)
    subprocess.run(["c++", "-std=c++20", str(cpp), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print("Selection button regression checks passed")
