"""Run with python3 test/test_list_viewport.py (requires a host C++ compiler)."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
# Compile the production method without the activity's hardware dependencies.
source = (ROOT / "src/activities/UiListActivity.cpp").read_text()
method = source.split("void UiListActivity::syncListViewport(", 1)[1]
method = "void UiListActivity::syncListViewport(" + method.split(
    "\nvoid UiListActivity::drawChrome", 1
)[0]
harness = r"""
#include <components/lists/list.h>
#include <cassert>
namespace fui = freeink::ui;
struct UiScreen {
  int16_t height = 80;
  void syncListViewport(fui::ListNav& nav, fui::ListProps& props, int count, int offset) {
    nav.syncToProps({0, 0, 200, height}, 20, 0, count, props, offset);
  }
};
struct UiListActivity {
  fui::ListNav nav;
  int count = 16;
  fui::ListNav& activeNav() { return nav; }
  int listCount() const { return count; }
  void syncListViewport(UiScreen&, fui::ListProps&, int = 0);
};
""" + method + r"""
int main() {
  for (int offset : {0, 1}) {
    UiListActivity activity;
    UiScreen screen;
    fui::ListProps props;
    auto& nav = activity.nav;
    nav.reset(offset);
    activity.syncListViewport(screen, props, offset);
    nav.onListRendered(0, 4, true);
    nav.requestScroll(4);
    activity.syncListViewport(screen, props, offset);
    assert(nav.top == 4 && props.topIndex == 4 && nav.selected == offset);
    nav.onListRendered(4, 4, false);
    nav.selected = 5 + offset;
    nav.requestScroll(-4);
    activity.syncListViewport(screen, props, offset);
    assert(nav.top == 0 && props.topIndex == 0 && nav.selected == 5 + offset);

    // Wrapped settings rows: button follow must preserve the measured viewport
    // even when the fixed-height estimate says the entire list fits.
    activity.count = 6;
    screen.height = 160;
    nav.onListRendered(2, 3, true);
    nav.drawnCount = 6;
    nav.requestSelection(3 + offset);
    activity.syncListViewport(screen, props, offset);
    assert(nav.top == 2 && props.topIndex == 2);
  }
}
"""
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / "viewport.cpp"
    executable = Path(directory) / "viewport"
    cpp.write_text("#include <initializer_list>\n" + harness)
    subprocess.run([
        "c++", "-std=c++17", "-I",
        str(ROOT / "freeink-sdk/libs/ui/FreeInkUI/include"),
        str(ROOT / "freeink-sdk/libs/ui/FreeInkUI/src/FreeInkUI.cpp"),
        str(cpp), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
print("List viewport regression checks passed")
