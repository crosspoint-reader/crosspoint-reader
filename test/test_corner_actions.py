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

# Exercise the reader's actual tap and auto-turn branches with the timer due.
activity = (ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text()
loop_start = activity.index("void EpubReaderActivity::loop() {")
tap_start = activity.index("  if (!atEndOfBook && SETTINGS.touchReaderControls && mappedInput.hasTouch())", loop_start)
auto_start = activity.index("  if (automaticPageTurnActive) {", loop_start)
assert tap_start < auto_start
dispatch = activity[tap_start:activity.index("  if (showClippingMessage", auto_start)]
# Use the actual position capture and corner decision; the host fake stores bookmarks in memory.
toggle_start = activity.index("bool EpubReaderActivity::addBookmark(")
capture_start = activity.index("    RenderLock lock;", toggle_start)
capture = activity[capture_start:activity.index("    const int pageCount", capture_start)]
case_start = activity.index("    case CrossPointSettings::CORNER_ACTION_TOGGLE_BOOKMARK:", activity.index(
    "bool EpubReaderActivity::runCornerActionAt("))
bookmark_case = activity[case_start:activity.index("    case CrossPointSettings::CORNER_ACTION_BOOKMARKS:", case_start)]
dict_start = activity.index("    case CrossPointSettings::CORNER_ACTION_DICTIONARY:", case_start)
dictionary_case = activity[dict_start:activity.index("    case CrossPointSettings::CORNER_ACTION_FOOTNOTES:", dict_start)]
corner_feedback = activity[activity.index("  if (!hold)", dict_start):activity.index("\n}", dict_start)]
selection_start = activity.index("bool selectionWindowHasText(")
selection_text = activity[selection_start:activity.index("\n}", selection_start) + 2]

HARNESS = r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <set>
#include <utility>
#include <vector>
#define FREEINK_CAP_TOUCH 1

// Minimal page model so the real selectionWindowHasText() can be compiled and
// exercised; the reader's Page/PageLine provide the same getTag/valid/wordCount.
enum PageElementTag { TAG_PageLine, TAG_PageImage };
struct TextBlock {
  bool valid_ = true;
  int words = 1;
  bool valid() const { return valid_; }
  uint16_t wordCount() const { return static_cast<uint16_t>(words); }
};
struct PageElement {
  virtual ~PageElement() = default;
  virtual PageElementTag getTag() const = 0;
};
struct PageLine : PageElement {
  TextBlock block;
  PageElementTag getTag() const override { return TAG_PageLine; }
  const TextBlock* getBlock() const { return &block; }
};
struct PageImage : PageElement {
  PageElementTag getTag() const override { return TAG_PageImage; }
};
struct Page {
  std::vector<std::unique_ptr<PageElement>> elements;
};

""" + selection_text + r"""

struct GfxRenderer {
  int w = 480;
  int h = 800;
  int getScreenWidth() const { return w; }
  int getScreenHeight() const { return h; }
};

struct CrossPointSettings {
  bool touchReaderControls = true;
  char dictionaryName[2] = "d";
""" + enum_block + "\n" + default_fields + r"""
};
static CrossPointSettings SETTINGS;

""" + corner + r"""

GfxRenderer renderer;
struct MappedInputManager {
  enum class Button { Confirm, Back };
  bool hasTouch() const { return true; }
  bool wasScreenTapped(int& x, int& y) const { x = 479; y = 0; return true; }
  bool wasReleased(Button) const { return false; }
} mappedInput;
bool busy, locked, linkUnderCorner, automaticPageTurnActive = true;
constexpr bool atEndOfBook = false, section = true;
int lastPageTurnTime = 0, pageTurnDuration = 1;
int millis() { return 10; }
void requestUpdate() {}
void notePageTurn(bool, bool) {}
int linksFollowed, cornersRun, turns, haptics;
bool pageTurn(bool) { ++turns; return true; }
struct RenderLock {
  enum class Mode { Try };
  RenderLock() { assert(!locked); locked = true; }
  explicit RenderLock(Mode) { locked = !busy; }
  ~RenderLock() { locked = false; }
  bool ownsLock() const { return locked; }
  static bool peek() { return busy; }
};
namespace ReaderUtils { bool isTouchMenuGesture(GfxRenderer&, MappedInputManager&) { return false; } }
namespace haptic_feedback { void touchAction(bool = false) { ++haptics; } }
struct Link { const char* href; } currentPageLinks{"#note"};
constexpr int currentPageLinkMarginLeft = 0, currentPageLinkMarginTop = 0;
namespace EpubReaderUtils {
const Link* linkAtPoint(const Link& link, int x, int y, int, int) {
  assert(locked && x == 479 && y == 0);
  return linkUnderCorner ? &link : nullptr;
}
}
void navigateToHref(const std::string& href, bool) { assert(!locked && href == "#note"); ++linksFollowed; }
bool runCornerActionAt(int x, int y, bool hold) {
  assert(!locked);
  if (!cornerActionAt(renderer, x, y, hold)) return false;
  ++cornersRun;
  return true;
}
void tapWithTimerDue() {
""" + dispatch + r"""
}

struct BookmarkReader {
  enum class SelectionResult { Unavailable, Consumed, Opened };
  struct ClipSelectionActivity { enum class Mode { Lookup }; };
  SelectionResult lookup = SelectionResult::Unavailable;
  SelectionResult startClipSelection(int, int, ClipSelectionActivity::Mode) { return lookup; }
  bool automaticPageTurnActive = true;
  int pendingManualTurn = 1;
  struct { int currentPage = 0; } loaded;
  decltype(loaded)* section = &loaded;
  bool epub = true, showBookmarkMessage = false;
  int currentSpineIndex = 0;
  struct { int spineIndex, pageNumber; } bookmarkMessagePosition{};
  std::set<std::pair<int, int>> bookmarks;

  bool addBookmark(bool showConfirmation) {
    {
""" + capture + r"""
      const auto page = std::make_pair(currentSpineIndex, currentPage);
      if (!bookmarks.erase(page)) bookmarks.insert(page);
    }
    showBookmarkMessage = showConfirmation;
    return true;
  }
  bool tap(int action = CrossPointSettings::CORNER_ACTION_TOGGLE_BOOKMARK, bool hold = false) {
    switch (action) {
""" + bookmark_case + dictionary_case + r"""
    default: return false;
    }
""" + corner_feedback + r"""
  }
};

int main() {
  BookmarkReader reader;
  assert(reader.tap() && reader.tap());  // repeat must not undo the add
  ++reader.loaded.currentPage;
  assert(reader.tap());  // previous confirmation is still visible
  assert(reader.bookmarks.count({0, 0}) && reader.bookmarks.count({0, 1}));
  ++reader.currentSpineIndex;
  assert(reader.tap() && reader.bookmarks.count({1, 1}));  // same page number in another chapter
  reader.showBookmarkMessage = false;
  assert(reader.tap() && reader.tap() && !reader.bookmarks.count({1, 1}));  // repeat after removal
  for (bool hold : {false, true}) {
    BookmarkReader dictionary;
    const int before = haptics;
    const int action = CrossPointSettings::CORNER_ACTION_DICTIONARY;
    assert(!dictionary.tap(action, hold));  // unavailable: allow normal fallback
    dictionary.lookup = BookmarkReader::SelectionResult::Consumed;
    assert(dictionary.tap(action, hold) && dictionary.automaticPageTurnActive && haptics == before);
    dictionary.lookup = BookmarkReader::SelectionResult::Opened;
    assert(dictionary.tap(action, hold) && haptics == before + 1);
    assert(dictionary.automaticPageTurnActive == hold);
  }
  haptics = 0;

  // Corner Look Up must not report success when the window has nothing to
  // select: the picker would cancel on entry.
  {
    std::vector<std::unique_ptr<Page>> window;
    assert(!selectionWindowHasText(window));  // empty window
    window.push_back(std::make_unique<Page>());  // page with no elements
    assert(!selectionWindowHasText(window));
    window.back()->elements.push_back(std::make_unique<PageImage>());
    assert(!selectionWindowHasText(window));  // image-only page
    auto line = std::make_unique<PageLine>();
    line->block.valid_ = false;
    line->block.words = 5;
    window.back()->elements.push_back(std::move(line));
    assert(!selectionWindowHasText(window));  // invalid block
    line = std::make_unique<PageLine>();
    line->block.words = 0;
    window.back()->elements.push_back(std::move(line));
    assert(!selectionWindowHasText(window));  // empty line
    auto second = std::make_unique<Page>();  // text on a later page in the window
    auto words = std::make_unique<PageLine>();
    words->block.words = 2;
    second->elements.push_back(std::move(words));
    window.push_back(std::move(second));
    assert(selectionWindowHasText(window));
  }

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
  busy = linkUnderCorner = true;
  tapWithTimerDue();
  assert(!linksFollowed && !cornersRun && !turns && !haptics);
  busy = false;
  tapWithTimerDue();
  assert(linksFollowed == 1 && !cornersRun && !turns && haptics == 1);
  linkUnderCorner = false;
  tapWithTimerDue();
  assert(linksFollowed == 1 && cornersRun == 1 && !turns);
  SETTINGS.cornerTopRightTap = CrossPointSettings::CORNER_ACTION_DISABLED;
  tapWithTimerDue();
  assert(linksFollowed == 1 && cornersRun == 1 && turns == 1);
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

# The Controls menu groups: the four corner settings must live on their own
# Corner screen, not on the Taps and gestures screen.
settings_activity = (ROOT / "src/activities/settings/SettingsActivity.cpp").read_text()
group_fn_start = settings_activity.index("bool belongsToControlsGroup(")
group_fn = settings_activity[group_fn_start:settings_activity.index("\n}", group_fn_start) + 2]

GROUPS_HARNESS = r"""
#include <cassert>
#include <cstdint>
#include <cstring>

enum class ControlsGroup : uint8_t { Root, Power, Front, Side, Touch, Corner };
struct CrossPointSettings {
  uint8_t longPressButtonBehavior = 0, pwrBtnFootnoteBack = 0, frontButtonFollowOrientation = 0,
          backShortToFileBrowser = 0, sideButtonLayout = 0, sideButtonFollowOrientation = 0,
          touchReaderControls = 0, pageTurnGesture = 0, previousPageGesture = 0, showReaderMenu = 0,
          cornerTopLeftTap = 0, cornerTopLeftHold = 0, cornerTopRightTap = 0, cornerTopRightHold = 0,
          unrelated = 0;
};
struct SettingInfo {
  uint8_t CrossPointSettings::* valuePtr = nullptr;
  const char* key = nullptr;
};

""" + group_fn + r"""

int main() {
  const SettingInfo corners[] = {{&CrossPointSettings::cornerTopLeftTap}, {&CrossPointSettings::cornerTopLeftHold},
                                 {&CrossPointSettings::cornerTopRightTap}, {&CrossPointSettings::cornerTopRightHold}};
  for (const SettingInfo& setting : corners) {
    assert(belongsToControlsGroup(setting, ControlsGroup::Corner));   // on the Corner screen
    assert(!belongsToControlsGroup(setting, ControlsGroup::Touch));   // not cluttering Taps and gestures
    assert(!belongsToControlsGroup(setting, ControlsGroup::Root));    // and not at the Controls root
  }
  const SettingInfo gestures[] = {{&CrossPointSettings::touchReaderControls}, {&CrossPointSettings::pageTurnGesture},
                                  {&CrossPointSettings::previousPageGesture}, {&CrossPointSettings::showReaderMenu}};
  for (const SettingInfo& setting : gestures) {
    assert(belongsToControlsGroup(setting, ControlsGroup::Touch));
    assert(!belongsToControlsGroup(setting, ControlsGroup::Corner));
  }
  const SettingInfo shared{&CrossPointSettings::longPressButtonBehavior};  // appears in both button groups
  assert(belongsToControlsGroup(shared, ControlsGroup::Front));
  assert(belongsToControlsGroup(shared, ControlsGroup::Side));
  assert(!belongsToControlsGroup(shared, ControlsGroup::Touch));
  const SettingInfo power{nullptr, "shortPwrBtn"};
  assert(belongsToControlsGroup(power, ControlsGroup::Power));
  assert(!belongsToControlsGroup(power, ControlsGroup::Root));
  const SettingInfo other{&CrossPointSettings::unrelated};
  assert(belongsToControlsGroup(other, ControlsGroup::Root));
  assert(!belongsToControlsGroup(other, ControlsGroup::Corner));
}
"""

with tempfile.TemporaryDirectory() as directory:
    tmp = Path(directory)
    (tmp / "groups.cpp").write_text(GROUPS_HARNESS)
    subprocess.run(
        ["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", str(tmp / "groups.cpp"), "-o", str(tmp / "groups")],
        check=True,
    )
    subprocess.run([str(tmp / "groups")], check=True)
print("Passed: corner geometry, bindings, tap dispatch, and the Corner actions menu group")
