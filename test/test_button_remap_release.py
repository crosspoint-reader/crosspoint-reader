"""Compile the production remapping loop with input/storage stubs."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/activities/settings/ButtonRemapActivity.cpp").read_text()
loop = "void ButtonRemapActivity::loop() {" + source.split(
    "void ButtonRemapActivity::loop() {", 1
)[1].split("void ButtonRemapActivity::render(", 1)[0]

harness = r"""
#include <cassert>
#include <cstdint>
#include <string>
unsigned long now = 2000;
unsigned long millis() { return now; }
constexpr uint8_t kRoleCount = 4;
struct Settings {
  enum { FRONT_HW_BACK, FRONT_HW_CONFIRM, FRONT_HW_LEFT, FRONT_HW_RIGHT };
  int frontButtonBack = 1, frontButtonConfirm = 0, frontButtonLeft = 3, frontButtonRight = 2;
  int saves = 0;
  void saveToFile() { ++saves; }
} SETTINGS;
using CrossPointSettings = Settings;
struct MappedInputManager {
  enum class Button { Back, Confirm, Left, Right, Up, Down };
  bool pressed[6] = {}, released[6] = {}, held[6] = {};
  int rawFront = -1;
  bool wasPressed(Button b) const { return pressed[int(b)]; }
  bool wasReleased(Button b) const { return released[int(b)]; }
  bool isPressed(Button b) const { return held[int(b)]; }
  int getPressedFrontButton() const { return rawFront; }
};
struct ButtonRemapActivity {
  uint8_t currentStep = 0;
  uint8_t tempMapping[4] = {255, 255, 255, 255};
  unsigned long errorUntil = 0;
  std::string errorMessage;
  MappedInputManager mappedInput;
  int finishes = 0, updates = 0, applies = 0;
  void finish() { ++finishes; }
  void requestUpdate() { ++updates; }
  void applyTempMapping() { ++applies; }
  bool validateUnassigned(uint8_t raw) const {
    for (auto assigned : tempMapping) if (assigned == raw) return false;
    return true;
  }
  void loop();
};
struct RenderLock { explicit RenderLock(ButtonRemapActivity&) {} };
""" + loop + r"""
int main() {
  using Button = MappedInputManager::Button;
  for (const auto side : {Button::Up, Button::Down}) {
    SETTINGS = Settings{};
    ButtonRemapActivity wizard;
    wizard.mappedInput.pressed[int(side)] = true;
    wizard.mappedInput.held[int(side)] = true;
    wizard.loop();
    assert(wizard.finishes == 0 && SETTINGS.saves == 0);
    wizard.mappedInput.pressed[int(side)] = false;
    for (int i = 0; i < 100; ++i) wizard.loop();
    assert(wizard.finishes == 0 && SETTINGS.saves == 0);
    wizard.mappedInput.held[int(side)] = false;
    wizard.mappedInput.released[int(side)] = true;
    wizard.loop();
    assert(wizard.finishes == 1);
    assert(SETTINGS.saves == (side == Button::Up ? 1 : 0));
    assert(SETTINGS.frontButtonBack == (side == Button::Up ? 0 : 1));
    assert(SETTINGS.frontButtonConfirm == (side == Button::Up ? 1 : 0));
    assert(SETTINGS.frontButtonLeft == (side == Button::Up ? 2 : 3));
    assert(SETTINGS.frontButtonRight == (side == Button::Up ? 3 : 2));
  }
  // The final raw assignment may currently have any logical role.
  for (int logical = 0; logical < 4; ++logical) {
    SETTINGS = Settings{};
    ButtonRemapActivity wizard;
    for (int raw = 0; raw < 4; ++raw) {
      wizard.mappedInput.rawFront = raw;
      wizard.mappedInput.held[logical] = true;
      wizard.loop();
      wizard.mappedInput.rawFront = -1;
      if (raw < 3) wizard.mappedInput.held[logical] = false;
    }
    assert(wizard.currentStep == 4 && wizard.applies == 0 && wizard.finishes == 0);
    for (int i = 0; i < 100; ++i) wizard.loop();
    assert(wizard.applies == 0 && SETTINGS.saves == 0 && wizard.finishes == 0);
    wizard.mappedInput.held[logical] = false;
    wizard.mappedInput.released[logical] = true;
    wizard.loop();
    assert(wizard.applies == 1 && SETTINGS.saves == 1 && wizard.finishes == 1);
    // A valid completed mapping has four distinct raw indices.
    for (int i = 0; i < 4; ++i) assert(wizard.tempMapping[i] == i);
  }
  ButtonRemapActivity duplicate;
  duplicate.mappedInput.rawFront = 2;
  duplicate.loop();
  duplicate.loop();
  assert(duplicate.currentStep == 1 && duplicate.finishes == 0);
}
"""

with tempfile.TemporaryDirectory(prefix="button-remap-release-") as directory:
    path = Path(directory)
    (path / "test.cpp").write_text(harness)
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    str(path / "test.cpp"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("Button remapping release tests passed")
