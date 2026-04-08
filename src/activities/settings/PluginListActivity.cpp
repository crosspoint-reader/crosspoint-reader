#include "PluginListActivity.h"

#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "PluginDetailActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "plugin/PluginRegistry.h"

void PluginListActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void PluginListActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (PluginRegistry::count() > 0) {
      startActivityForResult(
          std::make_unique<PluginDetailActivity>(renderer, mappedInput, selectedIndex),
          [this](const ActivityResult&) { requestUpdate(); });
    }
    return;
  }

  buttonNavigator.onNextRelease([this] {
    if (PluginRegistry::count() > 0) {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, PluginRegistry::count());
      requestUpdate();
    }
  });

  buttonNavigator.onPreviousRelease([this] {
    if (PluginRegistry::count() > 0) {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, PluginRegistry::count());
      requestUpdate();
    }
  });
}

void PluginListActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_PLUGINS),
                 CROSSPOINT_VERSION);

  const int itemCount = PluginRegistry::count();

  if (itemCount == 0) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, tr(STR_PLUGIN_NO_PLUGINS));
  } else {
    GUI.drawList(
        renderer,
        Rect{0, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing, pageWidth,
             pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.buttonHintsHeight +
                           metrics.verticalSpacing * 2)},
        itemCount, selectedIndex,
        [](int index) -> std::string {
          const auto* p = PluginRegistry::get(index);
          if (!p) return "";
          return p->name;
        },
        nullptr, nullptr,
        [](int index) -> std::string {
          const auto* p = PluginRegistry::get(index);
          if (!p) return "";
          if (!PluginRegistry::isCompatible(index)) {
            char buf[64];
            snprintf(buf, sizeof(buf), "! %s", tr(STR_PLUGIN_INCOMPATIBLE));
            return buf;
          }
          return PluginRegistry::isEnabled(p->id) ? tr(STR_PLUGIN_ENABLED) : tr(STR_PLUGIN_DISABLED);
        },
        true);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
