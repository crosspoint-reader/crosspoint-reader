#include "MenuListActivity.h"

#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"

void MenuListActivity::initMenuList() {
  const int count = static_cast<int>(menuItems.size());
  const auto pred = UITheme::makeSelectablePredicate(count, [this](int i) { return menuItems[i].getTitle(); });
  buttonNavigator.setSelectablePredicate(pred, count);
  if (count > 0 && !pred(selectedIndex)) {
    selectedIndex = buttonNavigator.nextIndex(selectedIndex);
  }
}

void MenuListActivity::onEnter() {
  Activity::onEnter();
  initMenuList();
  requestUpdate();
}

void MenuListActivity::handleNavigation() {
  buttonNavigator.onNext([this] {
    selectedIndex = buttonNavigator.nextIndex(selectedIndex);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectedIndex = buttonNavigator.previousIndex(selectedIndex);
    requestUpdate();
  });
}

void MenuListActivity::toggleCurrentItem() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(menuItems.size())) return;
  const auto& item = menuItems[selectedIndex];
  if (item.isSeparator) return;

  if (item.type == SettingType::ACTION) {
    onActionSelected(selectedIndex);
    return;
  }

  menuItems[selectedIndex].toggleValue();
  onSettingToggled(selectedIndex);
  requestUpdate();
}

std::string MenuListActivity::getItemValueString(int index) const { return menuItems[index].getDisplayValue(); }

void MenuListActivity::drawMenuList(const Rect& rect) {
  const int count = static_cast<int>(menuItems.size());
  GUI.drawList(
      renderer, rect, count, selectedIndex, [this](int index) { return menuItems[index].getTitle(); }, nullptr, nullptr,
      [this](int index) { return getItemValueString(index); }, true);
}

void MenuListActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBackPressed();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    toggleCurrentItem();
    return;
  }
  handleNavigation();
}
