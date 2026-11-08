"""Check production group eligibility methods without the activity's graphics dependencies."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/activities/library/LibraryListActivity.cpp").read_text()
methods = []
for start, end in [("void LibraryListActivity::syncIndexState()", "void LibraryListActivity::resetAfterRebuild()"),
                   ("bool LibraryListActivity::groupable()", "// Return the row's group"),
                   ("bool LibraryListActivity::tabEnabled(", "const char* LibraryListActivity::tabLabel("),
                   ("void LibraryListActivity::stepTab(", "void LibraryListActivity::onTabAction(")]:
    methods.append(source[source.index(start):source.index(end, source.index(start))])
harness = r'''
#include <cassert>
#define LOG_ERR(...) ((void)0)
namespace library { enum class GroupKind { None, Series }; }
enum class Order { Recent, Title, Group };
constexpr int RECENT_TAB = 0, TITLE_TAB = 1, AUTHOR_TAB = 2, GROUP_TAB = 3;
Order orderForTab(int tab, unsigned) { return tab == GROUP_TAB ? Order::Group : Order::Title; }
bool isRecentSort(Order order) { return order == Order::Recent; }
bool isGroupSort(Order order) { return order == Order::Group; }
struct Index {
  bool ranks = false, groups = false, open = true;
  bool isOpen() const { return open; }
  bool ranksDegraded() const { return ranks; }
  bool dedupDegraded() const { return false; }
  bool groupsDegraded() const { return !groups; }
  bool hasGroups() const { return open && groups; }
  library::GroupKind groupKind() const { return library::GroupKind::Series; }
};
struct LibraryListActivity {
  Index index;
  bool degraded = false, groupTabAvailable = false;
  library::GroupKind groupTabKind = library::GroupKind::None;
  Order sortOrder = Order::Group;
  int count = 2, activeTabIndex = AUTHOR_TAB;
  unsigned descendingTabs = 0;
  int tabCount() const { return groupTabAvailable ? 4 : 3; }
  int activeTab() const { return activeTabIndex; }
  bool tabEnabled(int) const;
  void stepTab(int);
  void selectTab(int tab, bool) { assert(tabEnabled(tab)); activeTabIndex = tab; }
  int bookRowCount() const { return count; }
  void clearGroupCache() {}
  void syncIndexState();
  bool groupable() const;
};
''' + "\n".join(methods) + r'''
int main() {
  for (bool ranks : {false, true}) {
    for (bool groups : {false, true}) {
      LibraryListActivity activity;
      activity.index.ranks = ranks;
      activity.index.groups = groups;
      activity.syncIndexState();
      assert(activity.groupTabAvailable == groups);
      assert(activity.degraded == (ranks && !groups));
      assert(activity.groupable() == groups);
      assert(activity.tabEnabled(AUTHOR_TAB) == !(ranks && groups));
      if (ranks && groups) {
        assert(activity.activeTabIndex == GROUP_TAB);
        activity.activeTabIndex = TITLE_TAB;
        activity.stepTab(1);
        assert(activity.activeTabIndex == GROUP_TAB);
        activity.stepTab(-1);
        assert(activity.activeTabIndex == TITLE_TAB);
      }
      activity.sortOrder = Order::Title;
      assert(activity.groupable() == !ranks);
      activity.sortOrder = Order::Recent;
      assert(!activity.groupable());
      activity.sortOrder = Order::Group;
      activity.count = 0;
      assert(!activity.groupable());
    }
  }
}
'''
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / "navigation.cpp"
    binary = Path(directory) / "navigation"
    cpp.write_text("#include <initializer_list>\n" + harness)
    subprocess.run(["c++", "-std=c++20", str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Library group navigation regression passed")
