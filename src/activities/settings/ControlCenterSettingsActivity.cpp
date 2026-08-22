#include "ControlCenterSettingsActivity.h"

#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
// Row order mirrors the control center's tile grid, so hiding a row and looking
// at the panel line up.
constexpr StrId menuNames[] = {
    StrId::STR_NIGHT_MODE,   StrId::STR_FORCE_REFRESH,     StrId::STR_ORIENTATION,
    StrId::STR_TOUCH_TOGGLE, StrId::STR_SCREENSHOT_BUTTON, StrId::STR_SLEEP,
};

// The flag each row drives, same order.
constexpr uint8_t CrossPointSettings::* menuFlags[] = {
    &CrossPointSettings::ccTileNightMode, &CrossPointSettings::ccTileRefresh,    &CrossPointSettings::ccTileOrientation,
    &CrossPointSettings::ccTileTouch,     &CrossPointSettings::ccTileScreenshot, &CrossPointSettings::ccTileSleep,
};
}  // namespace

ControlCenterSettingsActivity::ControlCenterSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("ControlCenterSettings", renderer, mappedInput) {}

void ControlCenterSettingsActivity::onEnter() {
  UiListActivity::onEnter();

  for (int i = 0; i < ITEM_COUNT; i++) {
    rowItems_[i].label = I18N.get(menuNames[i]);
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

const char* ControlCenterSettingsActivity::headerTitle() const { return tr(STR_CUSTOMISE_CONTROL_CENTER); }

void ControlCenterSettingsActivity::activateIndex(const int index) {
  if (index < 0 || index >= ITEM_COUNT) return;
  nav.selected = index;
  uint8_t& flag = SETTINGS.*(menuFlags[index]);
  flag = flag ? 0 : 1;
  SETTINGS.saveToFile();
  requestUpdate();
}

void ControlCenterSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // tr() hands back a pointer into the static string table, so the row can
  // hold it directly — no per-render std::string to build and free.
  for (int i = 0; i < ITEM_COUNT; i++) {
    rowItems_[i].value = SETTINGS.*(menuFlags[i]) ? tr(STR_SHOW) : tr(STR_HIDE);
  }

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(ITEM_COUNT);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;  // also the explicitly-set marker, see SettingsActivity
  syncListViewport(screen, props);
  screen.list(props);
}
