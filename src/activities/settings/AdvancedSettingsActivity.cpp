#include "AdvancedSettingsActivity.h"

#include <utility>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "util/PluginEvents.h"

namespace fui = freeink::ui;

bool AdvancedSettingsActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void AdvancedSettingsActivity::activateIndex(const int index) {
  if (index != 0 || optionPopup.isActive()) return;
  nav.selected = 0;
  app.clearTapFlash();

  static constexpr StrId OPTIONS[] = {StrId::STR_ENABLED, StrId::STR_DISABLED, StrId::STR_CANCEL};
  const int current = SETTINGS.pluginsEnabled ? 0 : 1;
  optionPopup.show(StrId::STR_PLUGIN_SYSTEM, OPTIONS, 3, current, [this](const int selected) {
    if (selected == 2) return;
    setPluginSystemEnabled(selected == 0);
  });
  requestUpdate();
}

void AdvancedSettingsActivity::setPluginSystemEnabled(const bool enabled) {
  const uint8_t value = enabled ? 1 : 0;
  const bool changed = SETTINGS.pluginsEnabled != value;
  if (changed) {
    SETTINGS.pluginsEnabled = value;
    if (enabled) SETTINGS.pluginHubPromptHidden = 0;
    SETTINGS.saveToFile();
  }

  // Rebuild immediately so disabling stops event delivery in this session and
  // enabling restores subscriptions without requiring a reboot.
  pluginevents::refreshSubscriptions();
  requestUpdate();
}

void AdvancedSettingsActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

void AdvancedSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  row.label = tr(STR_PLUGIN_SYSTEM);
  row.value = SETTINGS.pluginsEnabled ? tr(STR_ENABLED) : tr(STR_DISABLED);
  row.actionValue = 0;

  fui::ListProps props;
  props.items = &row;
  props.count = 1;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}
