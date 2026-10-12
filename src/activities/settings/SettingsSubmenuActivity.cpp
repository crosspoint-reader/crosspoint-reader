#include "SettingsSubmenuActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SettingActionDispatch.h"
#include "components/UITheme.h"
#include "fontIds.h"

void SettingsSubmenuActivity::onEnter() {
  MenuListActivity::onEnter();
  needsHalfRefresh = true;
}

void SettingsSubmenuActivity::onActionSelected(int index) {
  const auto& setting = menuItems[index];
  if (setting.isSeparator) return;

  if (setting.type == SettingType::ACTION) {
    if (tryOpenSliderFor(setting.action, [this] {
          needsHalfRefresh = true;
          requestUpdate();
        })) {
      return;
    }

    auto activity = createActivityForAction(setting.action, renderer, mappedInput);
    if (activity) {
      const bool usedWifi = activity->usesWifi();
      startActivityForResult(std::move(activity), [this, usedWifi](const ActivityResult&) {
        CrossPointSettings::normalizeDependentSettings(SETTINGS);
        SETTINGS.saveToFile();
        if (usedWifi) restartToSettingsIfRadioLeftOn();
        needsHalfRefresh = true;
        requestUpdate();
      });
      return;
    }

    // Submenu items with no associated SettingAction still need to be routed through
    // the parent activity by nameId.
    setResult(MenuResult{-1, static_cast<int>(setting.nameId)});
    finish();
    return;
  }

  onSettingToggled(index);
}

std::string SettingsSubmenuActivity::getItemValueString(int index) const {
  const auto& item = menuItems[index];
  if (item.type == SettingType::ACTION && item.action != SettingAction::Submenu) {
    return item.stringGetter ? item.getDisplayValue() : std::string{};
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
    auto selector = createSelectorActivity(setting, renderer, mappedInput);
    if (selector) {
      startActivityForResult(std::move(selector), [this](const ActivityResult&) {
        if (persistSettingsOnChange) SETTINGS.saveToFile();
        needsHalfRefresh = true;
        requestUpdate();
      });
    }
    return;
  }

  MenuListActivity::toggleCurrentItem();
}

void SettingsSubmenuActivity::onSettingToggled(int /*index*/) {
  if (persistSettingsOnChange) SETTINGS.saveToFile();
}

void SettingsSubmenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = listContentRect();

  GUI.drawHeader(renderer, listHeaderRect(), I18N.get(titleId));

  const int contentTop = contentRect.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = contentRect.y + contentRect.height - contentTop - metrics.verticalSpacing;
  drawMenuList(Rect{contentRect.x, contentTop, contentRect.width, contentHeight});

  drawListHints();

  // Only spend the HALF if enough FAST refreshes have piled up since the last one to have
  // reintroduced ghosting. needsHalfRefresh is armed on entry AND on returning from every
  // child activity, so without this gate bouncing in and out of a submenu paid a 2186 ms HALF
  // per return against a FAST's 435 ms (X3-measured), even when the previous HALF was three
  // updates ago. Correctness HALFs are armed via setNextDisplayRefreshMode() and bypass this
  // entirely -- see GfxRenderer::ghostClearingHalfWorthwhile().
  const bool halfRefresh =
      renderer.ghostsUnderFastRefresh() && needsHalfRefresh && renderer.ghostClearingHalfWorthwhile();
  // Cleared whether or not it was spent, i.e. a gated request is DROPPED, not deferred.
  // Deferring would land the 2.2 s stall a few cursor moves into the screen, which reads as a
  // freeze; on entry it reads as the screen change it is. The ghosting cost of dropping is
  // marginal because the gate only fires when a HALF ran within the last few updates.
  needsHalfRefresh = false;
  renderer.displayBuffer(halfRefresh ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
