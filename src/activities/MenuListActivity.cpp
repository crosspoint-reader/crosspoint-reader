#include "MenuListActivity.h"

#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "settings/SettingActionDispatch.h"
#include "settings/SettingsSubmenuActivity.h"

namespace fui = freeink::ui;

MenuListActivity::MenuListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity(name, renderer, mappedInput), selectedIndex(nav.selected) {}

void MenuListActivity::initMenuList() {
  // Never open on a separator: step on to the next row that can hold the selection.
  const int count = static_cast<int>(menuItems.size());
  for (int tried = 0; tried < count && !isRowSelectable(selectedIndex); ++tried) {
    selectedIndex = (selectedIndex + 1) % count;
  }
}

bool MenuListActivity::isRowSelectable(const int index) const {
  return index >= 0 && index < static_cast<int>(menuItems.size()) && !menuItems[index].isSeparator;
}

void MenuListActivity::onEnter() {
  if (!submenusPrepared) {
    prepareSubmenus();
    SettingInfo::insertSubcategorySeparators(menuItems);
    submenusPrepared = true;
  }
  UiListActivity::onEnter();
  initMenuList();
  requestUpdate();
}

void MenuListActivity::activateIndex(const int index) {
  selectedIndex = index;
  toggleCurrentItem();
}

void MenuListActivity::toggleCurrentItem() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(menuItems.size())) return;
  const auto& item = menuItems[selectedIndex];
  if (item.isSeparator) return;

  // A row that asked for a full-screen picker gets one here, the same way SettingsActivity and
  // SettingsSubmenuActivity already give one to rows in their lists. Without this the flag was
  // simply ignored by every plain MenuListActivity screen, and the 86-option timezone row cycled
  // one press at a time.
  //
  // onSettingToggled() is what the result handler calls, so a subclass needs no new hook: the
  // branch that already reacted to an inline cycle reacts to a pick as well.
  //
  // SettingsSubmenuActivity overrides this function and returns before reaching here, so its
  // rows are still handled once, not twice.
  if (item.usesSelectorActivity) {
    auto selector = createSelectorActivity(item, renderer, mappedInput);
    if (selector) {
      const int index = selectedIndex;
      startActivityForResult(std::move(selector), [this, index](const ActivityResult&) {
        onSettingToggled(index);
        requestUpdate();
      });
    }
    return;
  }

  if (item.type == SettingType::ACTION) {
    if (item.action == SettingAction::Submenu) {
      openSubmenu(item);
      return;
    }
    onActionSelected(selectedIndex);
    return;
  }

  menuItems[selectedIndex].toggleValue();
  onSettingToggled(selectedIndex);
  requestUpdate();
}

std::string MenuListActivity::getItemValueString(int index) const { return menuItems[index].getDisplayValue(); }

void MenuListActivity::drawMenuList(const Rect& rect) {
  listRect = rect;
  renderUi();
  for (int pass = 0; nav.consumeRebuildNeeded() && pass < 8; ++pass) {
    renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
    renderUi();
  }
  publishListWindow();
}

void MenuListActivity::materializeListWindow() {
  const int count = listCount();
  windowFirst = static_cast<uint16_t>(std::max(0, std::min(nav.top, count)));
  windowCount = static_cast<uint16_t>(
      std::min(static_cast<size_t>(count - windowFirst), static_cast<size_t>(LIST_WINDOW_CAPACITY)));
  for (uint16_t offset = 0; offset < windowCount; ++offset) {
    const size_t index = windowFirst + offset;
    const auto& item = menuItems[index];
    windowLabels[offset] =
        item.isSeparator && item.nameId != StrId::STR_NONE_OPT ? I18N.get(item.nameId) : item.getTitle();
    // A TOGGLE row gets fui::list's switch rather than the words ON/OFF: ListItem::toggle
    // replaces the value slot with the same widget ToggleRowProps draws, and the switch visuals
    // are already inside list(), so this is a flag rather than new drawing code. The value string
    // is skipped for those rows -- list() ignores it when toggle is set, so building it would be
    // a std::string per row per render for nothing.
    const bool isToggle = !item.isSeparator && item.type == SettingType::TOGGLE;
    windowValues[offset] = isToggle ? std::string{} : getItemValueString(static_cast<int>(index));
    auto& row = windowItems[offset];
    row = {};
    row.label = windowLabels[offset].c_str();
    row.value = windowValues[offset].empty() ? nullptr : windowValues[offset].c_str();
    row.toggle = isToggle;
    row.toggleChecked = isToggle && item.getToggleState();
    row.actionValue = static_cast<int16_t>(index);
    row.enabled = !item.isSeparator;
    row.isHeader = item.isSeparator;
  }
}

void MenuListActivity::buildScreen(UiScreen& screen) {
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(listRect.y), static_cast<int16_t>(renderer.getScreenWidth() - (listRect.x + listRect.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (listRect.y + listRect.height)),
      static_cast<int16_t>(listRect.x)});

  auto props = listProps(screen);
  props.count = static_cast<uint16_t>(listCount());
  syncListViewport(screen, props);
  materializeListWindow();
  props.items = windowItems.data();
  props.itemsWindowFirst = windowFirst;
  props.itemsWindowCount = windowCount;
  noteListSelection(props);
  screen.list(props);
}

void MenuListActivity::prepareSubmenus() { SettingInfo::prepareSubmenus(menuItems, submenuData); }

void MenuListActivity::openSubmenu(const SettingInfo& submenuEntry) {
  auto it = std::find_if(submenuData.begin(), submenuData.end(),
                         [&submenuEntry](const SettingInfo::SubmenuData& d) { return d.id == submenuEntry.nameId; });
  if (it == submenuData.end()) return;

  startActivityForResult(
      std::make_unique<SettingsSubmenuActivity>(renderer, mappedInput, submenuEntry.nameId, it->items),
      [this](const ActivityResult&) { requestUpdate(); });
}

void MenuListActivity::loop() { UiListActivity::loop(); }

ListRowTap::Result MenuListActivity::selectListRow(const int index) {
  // Separators are already excluded by the band (recorded non-selectable, the same exclusion
  // isRowSelectable() makes for button navigation); declining here as well is belt and braces for
  // a list rebuilt between the render and the tap.
  if (index >= 0 && index < static_cast<int>(menuItems.size()) && menuItems[index].isSeparator) {
    return ListRowTap::Result::Rejected;
  }
  return ListRowTap::apply(index, static_cast<int>(menuItems.size()), selectedIndex);
}
