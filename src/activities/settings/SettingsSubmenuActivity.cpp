#include "SettingsSubmenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SettingActionDispatch.h"
#include "components/UITheme.h"
#include "fontIds.h"

void SettingsSubmenuActivity::onActionSelected(int index) {
  const auto& setting = menuItems[index];
  auto resultHandler = [this](const ActivityResult&) { SETTINGS.saveToFile(); };

  auto activity = createActivityForAction(setting.action, renderer, mappedInput);
  if (activity) startActivityForResult(std::move(activity), resultHandler);
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

  renderer.displayBuffer();
}
