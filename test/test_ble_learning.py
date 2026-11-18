"""Run with python3 test/test_ble_learning.py (requires a host C++ compiler)."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/activities/settings/BleSettingsActivity.cpp").read_text()
method = "void BleSettingsActivity::stepLearning()" + source.split(
    "void BleSettingsActivity::stepLearning()", 1
)[1].split("void BleSettingsActivity::keepLearned", 1)[0]
harness = r"""
#include <cassert>
#include <cstring>
#include <deque>
#include "BlePageTurner.h"
uint32_t clockMs = 100;
uint32_t millis() { return clockMs; }
#define tr(id) #id
namespace bleturner {
std::deque<Event> events;
Peer linked() { return {"01:02:03:04:05:06", "Remote"}; }
Status status() { Status result{}; result.connected = true; return result; }
bool pollEvent(Event& ev) {
  if (events.empty()) return false;
  ev = events.front(); events.pop_front(); return true;
}
}
struct BleSettingsActivity {
  bleturner::Action learnAction = bleturner::Action::NextPage;
  char learnAddr[18] = "01:02:03:04:05:06";
  uint32_t learnCode = 0, learnPressMs = 0, learnSinceMs = clockMs;
  const char* learnNote = nullptr;
  int saved = -1;
  void refresh() {}
  void requestUpdate() {}
  void keepLearned(bool hold) { saved = hold; learnAction = bleturner::Action::None; }
  void stepLearning();
};
""" + method + r"""
void press(BleSettingsActivity& screen) {
  bleturner::Event event{};
  event.code = 0x030002; event.pressed = true; event.atMs = clockMs;
  bleturner::events.push_back(event);
  screen.stepLearning();
}
void release(BleSettingsActivity& screen) {
  bleturner::Event event{};
  event.code = 0x030002; event.atMs = clockMs;
  bleturner::events.push_back(event);
  screen.stepLearning();
}
int main() {
  BleSettingsActivity hold;
  press(hold);
  clockMs += 2100; hold.stepLearning();
  assert(hold.saved == -1);
  clockMs += 900; release(hold);
  assert(hold.saved == 1);

  BleSettingsActivity tap;
  press(tap);
  clockMs += 100; release(tap);
  assert(tap.saved == 0);

  BleSettingsActivity noRelease;
  press(noRelease);
  clockMs += bleturner::kWaitMs - 1; noRelease.stepLearning();
  assert(noRelease.saved == -1);
  ++clockMs; noRelease.stepLearning();
  assert(noRelease.saved == 0);

  BleSettingsActivity noPress;
  clockMs += bleturner::kWaitMs; noPress.stepLearning();
  assert(noPress.saved == -1 && noPress.learnAction == bleturner::Action::None);
}
"""
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / "learning.cpp"
    executable = Path(directory) / "learning"
    cpp.write_text(harness)
    subprocess.run(["c++", "-std=c++20", "-I", str(ROOT / "lib/BlePageTurner/include"),
                    "-I", str(ROOT / "freeink-sdk/libs/network/BleKeyboardHost/include"),
                    str(cpp), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print("Bluetooth learning regression checks passed")
