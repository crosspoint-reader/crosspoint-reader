"""Exercise production popup navigation with host input/render stubs."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/components/OptionPopup.h").read_text()
source = "class OptionPopup" + source.split("class OptionPopup", 1)[1]
start = source.index("  bool processRender(")
end = source.index("  bool isActive()", start)
source = source[:start] + source[end:]
source = source.replace(" private:", " public:")
harness = r"""
#include <algorithm>
#include <atomic>
#include <cassert>
#include <functional>
#include <string>
#include <vector>
#include <FreeInkUI.h>
enum class StrId { Size };
struct { const char* get(StrId) { return "Size"; } } I18N;
struct MappedInputManager {
  enum class Button { NavPrevious, NavNext, Confirm, Back };
  enum class SwipeDir { None, Up, Down };
  SwipeDir swipe = SwipeDir::None;
  SwipeDir wasSwipe() { return swipe; }
  bool wasPressed(Button) { return false; }
  bool wasReleased(Button) { return false; }
};
freeink::ui::InputSnapshot touchSnapshotFrom(MappedInputManager&) { return {}; }
namespace haptic_feedback { void touchAction() {} }
""" + source + r"""
int main() {
  OptionPopup popup;
  std::vector<std::string> sizes(15, "pt");
  int chosen = -1;
  auto update = [] {};
  popup.show(StrId::Size, sizes, 14, [&](int i) { chosen = i; }, 5);
  assert(popup.firstOption == 10 && popup.selectedIndex == 14);
  popup.handleButtons(false, true, false, false, update);
  assert(popup.firstOption == 0 && popup.selectedIndex == 0);
  popup.handleButtons(true, false, false, false, update);
  assert(popup.firstOption == 10 && popup.selectedIndex == 14);
  popup.uiReady = true;
  MappedInputManager input;
  input.swipe = MappedInputManager::SwipeDir::Down;
  popup.handleInput(input, update);
  assert(popup.firstOption == 5 && popup.selectedIndex == 14 && chosen == -1);
  popup.handleInput(input, update);
  popup.handleInput(input, update);
  assert(popup.firstOption == 0);
  input.swipe = MappedInputManager::SwipeDir::Up;
  for (int i = 0; i < 4; ++i) popup.handleInput(input, update);
  assert(popup.firstOption == 10 && popup.isActive());
  popup.handleButtons(false, false, true, false, update);
  assert(chosen == 14 && !popup.isActive());
  popup.show(StrId::Size, sizes, 7, [](int) {});
  assert(popup.firstOption == 0 && popup.visibleOptions == 0);
  popup.show(StrId::Size, std::vector<std::string>(4, "pt"), 3, [](int) {}, 5);
  popup.uiReady = true;
  popup.handleInput(input, update);
  assert(popup.firstOption == 0);
}
"""
with tempfile.TemporaryDirectory() as temp:
    cpp = Path(temp) / "popup.cpp"
    cpp.write_text(harness)
    sdk = ROOT / "freeink-sdk/libs/ui/FreeInkUI"
    exe = Path(temp) / "popup"
    subprocess.run(["c++", "-std=c++17", "-I" + str(sdk / "include"), str(cpp),
                    str(sdk / "src/FreeInkUI.cpp"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("Option popup navigation passed")
