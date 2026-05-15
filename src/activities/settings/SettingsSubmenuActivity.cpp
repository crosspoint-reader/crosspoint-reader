#include "SettingsSubmenuActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "FontSelectionActivity.h"
#include "MappedInputManager.h"
#include "SdCardFontGlobals.h"
#include "SettingActionDispatch.h"
#include "components/UITheme.h"
#include "fontIds.h"

void SettingsSubmenuActivity::onEnter() {
  Activity::onEnter();
  needsHalfRefresh = true;
  initMenuList();
  requestUpdate();
}

void SettingsSubmenuActivity::onActionSelected(int index) {
  const auto& setting = menuItems[index];
  if (setting.isSeparator) return;

  if (setting.type == SettingType::ACTION) {
    MenuResult menuResult;
    if (setting.action != SettingAction::None) {
      menuResult.action = static_cast<int>(setting.action);
    } else {
      menuResult.nameId = static_cast<int>(setting.nameId);
    }
    setResult(ActivityResult(menuResult));
    finish();
    return;
  }

  onSettingToggled(index);
}

std::string SettingsSubmenuActivity::getItemValueString(int index) const {
  const auto& item = menuItems[index];
  if (item.type == SettingType::ACTION && item.action != SettingAction::Submenu) {
    return {};
  }
  if (itemValueStringOverride) {
    return itemValueStringOverride(item);
  }
  return MenuListActivity::getItemValueString(index);
}

void SettingsSubmenuActivity::toggleCurrentItem() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(menuItems.size())) return;
  const auto& setting = menuItems[selectedIndex];
  if (setting.isSeparator) return;

  if (setting.usesSelectorActivity) {
    const auto target = (setting.valueGetter == txtFontFamilyDynamicGetter) ? FontSelectionActivity::Target::TXT
                                                                            : FontSelectionActivity::Target::EPUB;
    startActivityForResult(std::make_unique<FontSelectionActivity>(renderer, mappedInput, target),
                           [this](const ActivityResult&) {
                             SETTINGS.saveToFile();
                             needsHalfRefresh = true;
                             requestUpdate();
                           });
    return;
  }

  MenuListActivity::toggleCurrentItem();
}

void SettingsSubmenuActivity::onSettingToggled(int /*index*/) { SETTINGS.saveToFile(); }

void SettingsSubmenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 I18N.get(titleId));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = contentRect.height - contentTop - metrics.verticalSpacing;
  drawMenuList(Rect{contentRect.x, contentTop, contentRect.width, contentHeight});

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const bool halfRefresh = gpio.deviceIsX3() && needsHalfRefresh;
  needsHalfRefresh = false;
  renderer.displayBuffer(halfRefresh ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
