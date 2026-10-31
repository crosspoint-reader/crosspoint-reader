"""Compile the production remapping loop with input/storage stubs."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/activities/settings/ButtonRemapActivity.cpp").read_text()
header = (ROOT / "src/activities/settings/ButtonRemapActivity.h").read_text()
exit_action = "enum class ExitAction" + header.split("enum class ExitAction", 1)[1].split(";", 1)[0] + ";"
loop = "void ButtonRemapActivity::loop() {" + source.split(
    "void ButtonRemapActivity::loop() {", 1
)[1].split("void ButtonRemapActivity::render(", 1)[0]
apply_mapping = "void ButtonRemapActivity::applyTempMapping() {" + source.split(
    "void ButtonRemapActivity::applyTempMapping() {", 1
)[1].split("bool ButtonRemapActivity::validateUnassigned(", 1)[0]
validate_unassigned = "bool ButtonRemapActivity::validateUnassigned(" + source.split(
    "bool ButtonRemapActivity::validateUnassigned(", 1
)[1].split("const char* ButtonRemapActivity::getRoleName(", 1)[0]
error_duration = "constexpr unsigned long kErrorDisplayMs" + source.split(
    "constexpr unsigned long kErrorDisplayMs", 1
)[1].split(";", 1)[0] + ";"

harness = r"""
#include <cassert>
#include <cstdint>
#include <string>
""" + error_duration + r"""
enum StrId { STR_ALREADY_ASSIGNED };
const char* tr(StrId id) { assert(id == STR_ALREADY_ASSIGNED); return "already assigned"; }
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
""" + exit_action + r"""
  ExitAction exitAction = ExitAction::None;
  uint8_t currentStep = 0;
  uint8_t tempMapping[4] = {255, 255, 255, 255};
  unsigned long errorUntil = 0;
  std::string errorMessage;
  MappedInputManager mappedInput;
  int finishes = 0, updates = 0;
  void finish() { ++finishes; }
  void requestUpdate() { ++updates; }
  void applyTempMapping();
  bool validateUnassigned(uint8_t raw);
  void loop();
};
struct RenderLock { explicit RenderLock(ButtonRemapActivity&) {} };
""" + loop + apply_mapping + validate_unassigned + r"""
void expectMapping(int back, int confirm, int left, int right) {
  assert(SETTINGS.frontButtonBack == back);
  assert(SETTINGS.frontButtonConfirm == confirm);
  assert(SETTINGS.frontButtonLeft == left);
  assert(SETTINGS.frontButtonRight == right);
}
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
    assert(wizard.currentStep == 4 && wizard.finishes == 0);
    expectMapping(1, 0, 3, 2);
    for (int i = 0; i < 100; ++i) wizard.loop();
    assert(SETTINGS.saves == 0 && wizard.finishes == 0);
    expectMapping(1, 0, 3, 2);
    wizard.mappedInput.held[logical] = false;
    wizard.mappedInput.released[logical] = true;
    wizard.loop();
    assert(SETTINGS.saves == 1 && wizard.finishes == 1);
    expectMapping(0, 1, 2, 3);
    // A valid completed mapping has four distinct raw indices.
    for (int i = 0; i < 4; ++i) assert(wizard.tempMapping[i] == i);
  }
  // Side actions must survive either release order, before or after the last assignment.
  for (const auto side : {Button::Up, Button::Down}) {
    for (bool frontReleasedFirst : {false, true}) {
      for (int assignments : {1, 4}) {
        for (int logical = 0; logical < 4; ++logical) {
          SETTINGS = Settings{};
          ButtonRemapActivity wizard;
          const int mapping[] = {2, 3, 0, 1};
          for (int i = 0; i < assignments; ++i) {
            wizard.mappedInput.rawFront = mapping[i];
            wizard.mappedInput.held[logical] = true;
            wizard.loop();
            wizard.mappedInput.rawFront = -1;
            if (i + 1 < assignments) wizard.mappedInput.held[logical] = false;
          }
          wizard.mappedInput.held[int(side)] = true;
          wizard.loop();
          assert(wizard.finishes == 0 && SETTINGS.saves == 0);
          if (frontReleasedFirst) {
            wizard.mappedInput.held[logical] = false;
            wizard.loop();
            assert(wizard.finishes == 0 && SETTINGS.saves == 0);
          }
          wizard.mappedInput.held[int(side)] = false;
          wizard.mappedInput.released[int(side)] = true;
          // A warning expiring in the same frame must not discard the side action.
          wizard.errorUntil = now - 1;
          wizard.errorMessage = "duplicate";
          wizard.loop();
          wizard.mappedInput.released[int(side)] = false;
          expectMapping(1, 0, 3, 2);
          if (!frontReleasedFirst) {
            for (int i = 0; i < 100; ++i) wizard.loop();
            assert(wizard.finishes == 0 && SETTINGS.saves == 0);
            // The first side action wins while the same front gesture is pending.
            const auto other = side == Button::Down ? Button::Up : Button::Down;
            wizard.mappedInput.released[int(other)] = true;
            wizard.loop();
            wizard.mappedInput.released[int(other)] = false;
            wizard.mappedInput.held[logical] = false;
          }
          wizard.loop();
          assert(wizard.finishes == 1);
          assert(SETTINGS.saves == (side == Button::Up ? 1 : 0));
          if (side == Button::Up) expectMapping(0, 1, 2, 3);
          else expectMapping(1, 0, 3, 2);
        }
      }
    }
  }
  ButtonRemapActivity duplicate;
  duplicate.mappedInput.rawFront = 2;
  duplicate.loop();
  duplicate.loop();
  assert(duplicate.currentStep == 1 && duplicate.finishes == 0);
  assert(duplicate.errorMessage == "already assigned");
  assert(duplicate.errorUntil == now + kErrorDisplayMs);
  ButtonRemapActivity sameRole;
  sameRole.currentStep = 2;
  sameRole.tempMapping[2] = 3;
  assert(sameRole.validateUnassigned(3));
  assert(sameRole.errorMessage.empty() && sameRole.errorUntil == 0);
  assert(sameRole.validateUnassigned(1));
}
"""

with tempfile.TemporaryDirectory(prefix="button-remap-release-") as directory:
    path = Path(directory)
    (path / "test.cpp").write_text(harness)
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    str(path / "test.cpp"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("Button remapping release tests passed")
