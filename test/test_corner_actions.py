"""Run with python3 test/test_corner_actions.py (requires a host C++20 compiler)."""

from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
corner_utils = (ROOT / "src/activities/reader/ReaderCornerUtils.h").read_text()
corner = corner_utils[corner_utils.index("constexpr int CORNER_TARGET_PERCENT"):
                      corner_utils.index("}  // namespace ReaderCornerUtils")]

# Read the persisted enum and defaults from the real header so a renumber or a
# changed default fails here instead of drifting silently.
settings_header = (ROOT / "src/CrossPointSettings.h").read_text()
enum_start = settings_header.index("enum CORNER_ACTION : uint8_t {")
enum_block = settings_header[enum_start:settings_header.index("};", enum_start) + 2]
defaults = re.findall(r"uint8_t (cornerTop\w+) = (CORNER_ACTION_\w+);", settings_header)
assert [name for name, _ in defaults] == ["cornerTopLeftTap", "cornerTopLeftHold", "cornerTopRightTap",
                                          "cornerTopRightHold"], defaults
default_fields = "\n".join(f"  uint8_t {name} = {value};" for name, value in defaults)

# Released ordinals are a persistence contract: this map pins the current ones.
expected_ordinals = {
    "CORNER_ACTION_DISABLED": 0,
    "CORNER_ACTION_TOGGLE_BOOKMARK": 1,
    "CORNER_ACTION_BOOKMARKS": 2,
    "CORNER_ACTION_SELECT_CHAPTER": 3,
    "CORNER_ACTION_READER_MENU": 4,
    "CORNER_ACTION_DICTIONARY": 5,
    "CORNER_ACTION_FOOTNOTES": 6,
    "CORNER_ACTION_NIGHT_MODE": 7,
    "CORNER_ACTION_GO_TO_PERCENT": 8,
    "CORNER_ACTION_SYNC": 9,
}
for name, value in expected_ordinals.items():
    assert re.search(rf"\b{name}\s*=\s*{value}\b", enum_block), (name, value)
# A retirement must not leave a hole: the label array is indexed by ordinal.
assert sorted(expected_ordinals.values()) == list(range(len(expected_ordinals))), expected_ordinals

# Every enum value needs a picker label; a new value must not ship blank.
settings_list = (ROOT / "src/SettingsList.cpp").read_text()
label_start = settings_list.index("CORNER_ACTION_VALUES = [")
label_block = settings_list[label_start:settings_list.index("return values;", label_start)]
labelled = set(re.findall(r"values\[CrossPointSettings::(CORNER_ACTION_\w+)\] = StrId::STR_\w+;", label_block))
declared = {name for name in re.findall(r"\b(CORNER_ACTION_\w+)\b", enum_block) if name != "CORNER_ACTION_COUNT"}
assert labelled == declared, (sorted(declared - labelled), sorted(labelled - declared))

HARNESS = r"""
#include <algorithm>
#include <cassert>
#include <cstdint>

struct GfxRenderer {
  int w = 480;
  int h = 800;
  int getScreenWidth() const { return w; }
  int getScreenHeight() const { return h; }
};

struct CrossPointSettings {
""" + enum_block + "\n" + default_fields + r"""
};
static CrossPointSettings SETTINGS;

""" + corner + r"""

int main() {
  // The target is 15% of the shorter dimension, identical in both orientations.
  GfxRenderer x4{480, 800};
  assert(cornerTargetSize(x4) == 72);
  GfxRenderer x3{528, 792};
  assert(cornerTargetSize(x3) == 79);
  GfxRenderer landscape{800, 480};
  assert(cornerTargetSize(landscape) == 72);
  GfxRenderer murphy{416, 240};
  assert(cornerTargetSize(murphy) == 36);

  // Top-left square is [0, 72) x [0, 72); top-right starts at width - 72.
  assert(topCornerAt(x4, 0, 0) == TopCorner::Left);
  assert(topCornerAt(x4, 71, 71) == TopCorner::Left);
  assert(topCornerAt(x4, 72, 0) == TopCorner::None);
  assert(topCornerAt(x4, 0, 72) == TopCorner::None);
  assert(topCornerAt(x4, 240, 0) == TopCorner::None);
  assert(topCornerAt(x4, 407, 0) == TopCorner::None);
  assert(topCornerAt(x4, 408, 0) == TopCorner::Right);
  assert(topCornerAt(x4, 479, 71) == TopCorner::Right);
  assert(topCornerAt(x4, 479, 72) == TopCorner::None);
  assert(topCornerAt(x4, -1, 0) == TopCorner::None);
  assert(topCornerAt(x4, 0, -1) == TopCorner::None);

  // Landscape keeps the same square size and follows the logical frame.
  assert(topCornerAt(landscape, 0, 0) == TopCorner::Left);
  assert(topCornerAt(landscape, 727, 0) == TopCorner::None);
  assert(topCornerAt(landscape, 728, 0) == TopCorner::Right);
  assert(topCornerAt(landscape, 799, 0) == TopCorner::Right);

  // The persisted defaults are part of the requirement: the top-left corner
  // starts disabled so an upgrade keeps the tap and word selection there.
  assert(cornerActionAt(x4, 0, 0, false) == CrossPointSettings::CORNER_ACTION_DISABLED);
  assert(cornerActionAt(x4, 0, 0, true) == CrossPointSettings::CORNER_ACTION_DISABLED);
  assert(cornerActionAt(x4, 479, 0, false) == CrossPointSettings::CORNER_ACTION_TOGGLE_BOOKMARK);
  assert(cornerActionAt(x4, 479, 0, true) == CrossPointSettings::CORNER_ACTION_BOOKMARKS);

  // Outside the corners every binding is Disabled, so page turns stay untouched.
  assert(cornerActionAt(x4, 240, 400, false) == CrossPointSettings::CORNER_ACTION_DISABLED);
  assert(cornerActionAt(x4, 240, 400, true) == CrossPointSettings::CORNER_ACTION_DISABLED);
  assert(cornerActionAt(x4, 0, 72, false) == CrossPointSettings::CORNER_ACTION_DISABLED);
  assert(cornerActionAt(x4, 407, 0, false) == CrossPointSettings::CORNER_ACTION_DISABLED);

  // A Disabled corner falls back to the normal tap zones.
  SETTINGS.cornerTopRightTap = CrossPointSettings::CORNER_ACTION_DISABLED;
  SETTINGS.cornerTopRightHold = CrossPointSettings::CORNER_ACTION_DISABLED;
  assert(cornerActionAt(x4, 479, 0, false) == CrossPointSettings::CORNER_ACTION_DISABLED);
  assert(cornerActionAt(x4, 479, 0, true) == CrossPointSettings::CORNER_ACTION_DISABLED);

  // Tap and hold resolve independently: a disabled tap must not hide a bound
  // hold, and any bound action is reachable through the same resolver.
  SETTINGS.cornerTopLeftHold = CrossPointSettings::CORNER_ACTION_SYNC;
  assert(cornerActionAt(x4, 0, 0, false) == CrossPointSettings::CORNER_ACTION_DISABLED);
  assert(cornerActionAt(x4, 0, 0, true) == CrossPointSettings::CORNER_ACTION_SYNC);
}
"""

with tempfile.TemporaryDirectory() as directory:
    tmp = Path(directory)
    (tmp / "corner.cpp").write_text(HARNESS)
    subprocess.run(
        ["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", str(tmp / "corner.cpp"), "-o", str(tmp / "corner")],
        check=True,
    )
    subprocess.run([str(tmp / "corner")], check=True)
print("Passed: top-corner geometry, 15% target sizing, and per-gesture action resolution")
