#include "UiTabListActivity.h"

#include <GfxRenderer.h>

#include <cassert>

#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
constexpr int16_t TOUCH_TAB_BAR_HEIGHT = 50;
}

UiTabListActivity::UiTabListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity(name, renderer, mappedInput) {}

void UiTabListActivity::onEnter() {
  // Size the per-tab state before the base resets activeNav() (which indexes
  // into it).
  tabNavs.assign(static_cast<size_t>(tabCount()), fui::ListNav{});
  UiListActivity::onEnter();
  app.on(ACTION_TAB, &UiTabListActivity::tabActionTrampoline, this);
}

fui::ListNav& UiTabListActivity::activeNav() {
  if (tabNavs.empty()) return nav;  // pre-onEnter fallback
  // Invariant: subclasses keep activeTab() inside [0, tabCount()), and
  // tabCount() does not change after onEnter() sized tabNavs.
  assert(activeTab() >= 0 && static_cast<size_t>(activeTab()) < tabNavs.size());
  return tabNavs[static_cast<size_t>(activeTab())];
}

int UiTabListActivity::ringPos() const {
  if (tabNavs.empty()) return 0;
  assert(activeTab() >= 0 && static_cast<size_t>(activeTab()) < tabNavs.size());
  return tabNavs[static_cast<size_t>(activeTab())].selected;
}

void UiTabListActivity::tabActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<UiTabListActivity*>(user);
  if (event.value < 0 || event.value >= self->tabCount()) return;
  self->onTabAction(event.value);
}

void UiTabListActivity::onRowAction(const fui::ActionEvent& event) {
  activeNav().selected = event.value + 1;  // ring position, not row index
  activateIndex(event.value);
}

void UiTabListActivity::moveRingTo(const int ringIndex) {
  auto& n = activeNav();
  n.selected = ringIndex;
  if (ringIndex == 0) {
    n.top = 0;
  } else {
    // Pull the viewport to the row (ring - 1); ListNav::follow reads
    // n.selected as a row index, so compute directly here.
    const uint16_t rows = n.visibleRows > 0 ? static_cast<uint16_t>(n.visibleRows) : 1;
    n.top = fui::listTopIndexFor(static_cast<int16_t>(ringIndex - 1), static_cast<uint16_t>(n.top < 0 ? 0 : n.top),
                                 rows, static_cast<uint16_t>(listCount()));
  }
  requestUpdate();
}

void UiTabListActivity::navigateButtons() {
  // Buttons walk the tab band (index 0) plus the rows (1..listCount).
  const int ringSize = listCount() + 1;
  buttonNavigator.onNextRelease([this, ringSize] { moveRingTo(ButtonNavigator::nextIndex(ringPos(), ringSize)); });
  buttonNavigator.onPreviousRelease(
      [this, ringSize] { moveRingTo(ButtonNavigator::previousIndex(ringPos(), ringSize)); });
  buttonNavigator.onNextContinuous([this] { stepTab(1); });
  buttonNavigator.onPreviousContinuous([this] { stepTab(-1); });
}

void UiTabListActivity::syncTabListViewport(UiScreen& screen, fui::ListProps& props, const bool hasSubtitle) {
  const int count = listCount();
  auto& n = activeNav();
  int16_t rowHeight = screen.theme().rowHeight;
  if (!mappedInput.hasTouch()) {
    // Non-touch hardware (X3/X4) keeps the original, denser per-theme row
    // height instead of FreeInkUI's touch-target-sized default (see
    // UiListActivity::syncListViewport, the non-tab counterpart of this).
    const auto& metrics = UITheme::getInstance().getMetrics();
    rowHeight = static_cast<int16_t>(hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight);
    // Wrapped (maxLines > 1) labels grow only their own row: list() sizes
    // wrapped items per-row, so the dense height stays for the rest.
    props.rowHeight = rowHeight;
  }
  const uint16_t rows = fui::listVisibleRows(screen.body(), rowHeight, screen.theme().listRowGap);
  n.visibleRows = rows > 0 ? rows : 1;
  if (n.followOnBuild) {
    // Screen entry / tab switch: show the tab's remembered selection, or the
    // top when the tab bar holds the focus.
    n.followOnBuild = false;
    n.top = n.selected > 0 ? static_cast<int>(fui::listTopIndexFor(
                                 static_cast<int16_t>(n.selected - 1), static_cast<uint16_t>(n.top < 0 ? 0 : n.top),
                                 static_cast<uint16_t>(n.visibleRows), static_cast<uint16_t>(count)))
                           : 0;
  }
  n.scrollBy(0, count);  // clamp to range
  // listCount() may shrink between passes (ring: 0 = tab band, 1..count = rows);
  // keep a stale ring selection from indexing past the new row count.
  if (n.selected > count) n.selected = count;
  props.topIndex = static_cast<uint16_t>(n.top);
  props.selectedIndex = static_cast<int16_t>(n.selected - 1);  // -1 = tab band focused
}

void UiTabListActivity::buildTabBar(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Tabs. The selected pill dims to a dither when the selection is down in
  // the list (the legacy focused/unfocused tab distinction).
  // Stack array, not a heap vector: this runs on every render and the tab
  // count is small and fixed.
  constexpr int MAX_TABS = 8;
  const int count = tabCount() < MAX_TABS ? tabCount() : MAX_TABS;
  fui::TabItem tabs[MAX_TABS];
  for (int i = 0; i < count; i++) {
    tabs[i].label = tabLabel(i);
    tabs[i].value = static_cast<int16_t>(i);
    tabs[i].selected = activeTab() == i;
  }
  fui::TabBarProps tabProps;
  tabProps.tabs = tabs;
  tabProps.count = static_cast<uint16_t>(count);
  tabProps.action = ACTION_TAB;
  tabProps.inputMask = fui::InputTouch;
  // Pill shape and label size are theme-driven. Lyra uses equal-width slots
  // with small labels so wide text (e.g. "Controls") still fits at large UI scales.
  // Full-slot (RoundedRaff): the pill fills its slot like the legacy layout
  // (slot minus a 4px frame, 8px clearance above the divider) with
  // body-size labels; zero horizontal contentInset disables the tabBar's
  // label-width shrink.
  const bool tabsFocused = ringPos() == 0;
  if (metrics.tabPillFullSlot) {
    tabProps.text = screen.theme().bodyText;
    tabProps.tabInset = fui::Insets{4, 4, 7, 4};
    tabProps.contentInset = fui::Insets{2, 0, 2, 0};
  } else {
    tabProps.text = screen.theme().smallText;
    tabProps.gap = static_cast<int16_t>(metrics.tabSpacing);
    // Unfocused state: no bottom inset, so the pill (and the 2px selected
    // underline drawn along its bottom edge) reaches the band's 1px divider —
    // legacy Lyra drew the underline sitting on that rule, not floating above.
    tabProps.tabInset = tabsFocused ? fui::Insets{2, 4, 4, 4} : fui::Insets{2, 4, 0, 4};
    tabProps.contentInset = fui::Insets{2, 0, 2, 0};
  }
  const int16_t tabLineHeight = screen.target().lineHeight(tabProps.text.font);
  const int16_t preferredTabHeight =
      mappedInput.hasTouch() ? TOUCH_TAB_BAR_HEIGHT : static_cast<int16_t>(metrics.tabBarHeight);
  const int16_t tabBand = preferredTabHeight > tabLineHeight + 10 ? preferredTabHeight : tabLineHeight + 10;
  // Legacy Lyra two-state treatment: with the selection on the tab band, the
  // band fills gray and the active tab is a solid pill; with the selection
  // down in the list, the band is plain and the active tab keeps a gray box
  // with an underline. The 1px rule under the band is always there.
  tabProps.divider = true;
  fui::StyleSet tabStyles;
  tabStyles.explicitlySet = true;
  tabStyles.normal.foreground = fui::Paint::solid(fui::Color::Black);
  if (tabsFocused) {
    tabStyles.selected.background = fui::Paint::solid(fui::Color::Black);
    tabStyles.selected.foreground = fui::Paint::solid(fui::Color::White);
    tabStyles.selected.radius = screen.theme().listRowRadius;
  } else if (metrics.tabPillFullSlot) {
    // Legacy RoundedRaff unfocused treatment: same pill, dimmed to dark gray,
    // text stays inverted; no underline.
    tabStyles.selected.background = fui::Paint::dither(fui::Color::DarkGray);
    tabStyles.selected.foreground = fui::Paint::solid(fui::Color::White);
    tabStyles.selected.radius = screen.theme().listRowRadius;
  } else {
    tabStyles.selected.background = fui::Paint::dither(fui::Color::LightGray);
    tabStyles.selected.foreground = fui::Paint::solid(fui::Color::Black);
    tabProps.selectedUnderline = 2;
  }
  // Focus/flash states keep the pill instead of falling back to an unset
  // (blank) style.
  tabStyles.focused = tabStyles.selected;
  tabStyles.active = tabStyles.selected;
  tabProps.tabStyles = tabStyles;
  const fui::Rect contentTabRect = screen.takeTop(tabBand);
  const fui::Rect frameRect = screen.frame().screen();
  // Tab chrome is a full-width screen band like the legacy GUI tab bar. The
  // remaining list content still stays inside the device safe area.
  const fui::Rect tabRect{frameRect.x, contentTabRect.y, frameRect.width, contentTabRect.height};
  // Focused band wash is the Lyra treatment; legacy RoundedRaff keeps the
  // band plain in both states.
  if (tabsFocused && !metrics.tabPillFullSlot) {
    screen.target().fill(tabRect, fui::Paint::dither(fui::Color::LightGray));
  }
  fui::tabBar(screen.frame(), tabRect, tabProps);
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
}
