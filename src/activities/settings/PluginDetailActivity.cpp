#include "PluginDetailActivity.h"

#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "plugin/PluginRegistry.h"

void PluginDetailActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void PluginDetailActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    const auto* plugin = PluginRegistry::get(pluginIndex);
    if (plugin && PluginRegistry::isCompatible(pluginIndex)) {
      const bool current = PluginRegistry::isEnabled(plugin->id);
      PluginRegistry::setEnabled(plugin->id, !current);
      requestUpdate();
    }
    return;
  }
}

void PluginDetailActivity::render(RenderLock&&) {
  const auto* plugin = PluginRegistry::get(pluginIndex);
  if (!plugin) {
    finish();
    return;
  }

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool compatible = PluginRegistry::isCompatible(pluginIndex);
  const bool enabled = PluginRegistry::isEnabled(plugin->id);

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, plugin->name, plugin->version);

  // Detail rows below the header
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int rowHeight = metrics.listRowHeight;
  const int leftPad = metrics.contentSidePadding;
  const int rightX = pageWidth - metrics.contentSidePadding;

  // Author
  renderer.drawText(UI_10_FONT_ID, leftPad, y, tr(STR_PLUGIN_AUTHOR), true, EpdFontFamily::BOLD);
  renderer.drawRightAlignedText(UI_10_FONT_ID, rightX, y, plugin->author);
  y += rowHeight;

  // Version
  renderer.drawText(UI_10_FONT_ID, leftPad, y, tr(STR_PLUGIN_VERSION), true, EpdFontFamily::BOLD);
  renderer.drawRightAlignedText(UI_10_FONT_ID, rightX, y, plugin->version);
  y += rowHeight;

  // Requires
  renderer.drawText(UI_10_FONT_ID, leftPad, y, tr(STR_PLUGIN_REQUIRES), true, EpdFontFamily::BOLD);
  char requiresBuf[64];
  snprintf(requiresBuf, sizeof(requiresBuf), ">= %s", plugin->minCpr);
  renderer.drawRightAlignedText(UI_10_FONT_ID, rightX, y, requiresBuf);
  y += rowHeight;

  // Status
  if (!compatible) {
    renderer.drawText(UI_10_FONT_ID, leftPad, y, tr(STR_PLUGIN_INCOMPATIBLE), true, EpdFontFamily::BOLD);
  } else {
    renderer.drawText(UI_10_FONT_ID, leftPad, y, enabled ? tr(STR_PLUGIN_ENABLED) : tr(STR_PLUGIN_DISABLED), true,
                      EpdFontFamily::BOLD);
  }
  y += rowHeight;

  // Description
  y += metrics.verticalSpacing;
  renderer.drawText(UI_10_FONT_ID, leftPad, y, plugin->description);
  y += rowHeight;

  // Plugin's own settings hook
  if (enabled && compatible && plugin->onSettingsRender) {
    plugin->onSettingsRender();
  }

  // Button hints
  const char* confirmLabel = "";
  if (compatible) {
    confirmLabel = enabled ? tr(STR_PLUGIN_DISABLE) : tr(STR_PLUGIN_ENABLE);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
