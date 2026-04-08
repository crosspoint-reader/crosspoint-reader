#include "PluginDetailActivity.h"

#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>
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
  if (pendingFinish_) {
    pendingFinish_ = false;
    finish();
    return;
  }

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
    pendingFinish_ = true;
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
  renderer.drawText(UI_10_FONT_ID, rightX - renderer.getTextWidth(UI_10_FONT_ID, plugin->author), y, plugin->author);
  y += rowHeight;

  // Version
  renderer.drawText(UI_10_FONT_ID, leftPad, y, tr(STR_PLUGIN_VERSION), true, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, rightX - renderer.getTextWidth(UI_10_FONT_ID, plugin->version), y, plugin->version);
  y += rowHeight;

  // Requires
  renderer.drawText(UI_10_FONT_ID, leftPad, y, tr(STR_PLUGIN_REQUIRES), true, EpdFontFamily::BOLD);
  char requiresBuf[64];
  snprintf(requiresBuf, sizeof(requiresBuf), ">= %s", plugin->minCpr);
  renderer.drawText(UI_10_FONT_ID, rightX - renderer.getTextWidth(UI_10_FONT_ID, requiresBuf), y, requiresBuf);
  y += rowHeight;

  // Status
  if (!compatible) {
    renderer.drawText(UI_10_FONT_ID, leftPad, y, tr(STR_PLUGIN_INCOMPATIBLE), true, EpdFontFamily::BOLD);
  } else {
    renderer.drawText(UI_10_FONT_ID, leftPad, y, enabled ? tr(STR_PLUGIN_ENABLED) : tr(STR_PLUGIN_DISABLED), true,
                      EpdFontFamily::BOLD);
  }
  y += rowHeight;

  // Description (with multi-line wrapping)
  y += metrics.verticalSpacing;
  {
    static constexpr int MAX_DESC_LINE_LEN = 256;
    const char* desc = plugin->description;
    if (desc) {
      const int maxWidth = rightX - leftPad;
      const char* lineStart = desc;
      while (*lineStart) {
        // Find how many characters fit on this line
        int len = strlen(lineStart);
        int fitLen = len;
        if (renderer.getTextWidth(UI_10_FONT_ID, lineStart) > maxWidth) {
          // Binary-search style: find last word boundary that fits
          fitLen = 0;
          int lastSpace = 0;
          for (int i = 0; i < len; i++) {
            char buf[MAX_DESC_LINE_LEN];
            int segLen = (i + 1 < (int)sizeof(buf)) ? i + 1 : (int)sizeof(buf) - 1;
            memcpy(buf, lineStart, segLen);
            buf[segLen] = '\0';
            if (renderer.getTextWidth(UI_10_FONT_ID, buf) > maxWidth) {
              break;
            }
            fitLen = i + 1;
            if (lineStart[i] == ' ') {
              lastSpace = fitLen;
            }
          }
          // Break at last space if possible
          if (lastSpace > 0) {
            fitLen = lastSpace;
          }
          if (fitLen == 0) fitLen = 1;  // Ensure at least one character per line
        }
        char lineBuf[MAX_DESC_LINE_LEN];
        int copyLen = (fitLen < (int)sizeof(lineBuf) - 1) ? fitLen : (int)sizeof(lineBuf) - 1;
        memcpy(lineBuf, lineStart, copyLen);
        lineBuf[copyLen] = '\0';
        // Trim trailing space
        if (copyLen > 0 && lineBuf[copyLen - 1] == ' ') {
          lineBuf[copyLen - 1] = '\0';
        }
        renderer.drawText(UI_10_FONT_ID, leftPad, y, lineBuf);
        y += rowHeight;
        lineStart += fitLen;
      }
    }
  }

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
