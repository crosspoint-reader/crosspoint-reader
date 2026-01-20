#include "ScreenComponents.h"

#include <GfxRenderer.h>

#include <cstdint>
#include <string>

#include "Battery.h"
#include "fontIds.h"

void ScreenComponents::drawBattery(const GfxRenderer& renderer, const int left, const int top,
                                   const bool showPercentage) {
  // Left aligned battery icon and percentage
  const uint16_t percentage = battery.readPercentage();
  const auto percentageText = showPercentage ? std::to_string(percentage) + "%" : "";
  renderer.drawText(SMALL_FONT_ID, left + 20, top, percentageText.c_str());

  // 1 column on left, 2 columns on right, 5 columns of battery body
  constexpr int batteryWidth = 15;
  constexpr int batteryHeight = 12;
  const int x = left;
  const int y = top + 6;

  // Top line
  renderer.drawLine(x + 1, y, x + batteryWidth - 3, y);
  // Bottom line
  renderer.drawLine(x + 1, y + batteryHeight - 1, x + batteryWidth - 3, y + batteryHeight - 1);
  // Left line
  renderer.drawLine(x, y + 1, x, y + batteryHeight - 2);
  // Battery end
  renderer.drawLine(x + batteryWidth - 2, y + 1, x + batteryWidth - 2, y + batteryHeight - 2);
  renderer.drawPixel(x + batteryWidth - 1, y + 3);
  renderer.drawPixel(x + batteryWidth - 1, y + batteryHeight - 4);
  renderer.drawLine(x + batteryWidth - 0, y + 4, x + batteryWidth - 0, y + batteryHeight - 5);

  // The +1 is to round up, so that we always fill at least one pixel
  int filledWidth = percentage * (batteryWidth - 5) / 100 + 1;
  if (filledWidth > batteryWidth - 5) {
    filledWidth = batteryWidth - 5;  // Ensure we don't overflow
  }

  renderer.fillRect(x + 2, y + 2, filledWidth, batteryHeight - 4);
}

ScreenComponents::PopupLayout ScreenComponents::drawPopup(const GfxRenderer& renderer, const char* message, const int y,
                                                          const int minWidth, const int minHeight) {
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, message, EpdFontFamily::BOLD);
  constexpr int margin = 16;
  const int contentWidth = textWidth > minWidth ? textWidth : minWidth;
  const int x = (renderer.getScreenWidth() - contentWidth - margin * 2) / 2;
  const int w = contentWidth + margin * 2;
  const int contentHeight = renderer.getLineHeight(UI_12_FONT_ID) + margin * 2;
  const int h = contentHeight >= minHeight ? contentHeight : minHeight;
  renderer.fillRect(x - 2, y - 2, w + 4, h + 4, true);
  renderer.fillRect(x + 2, y + 2, w - 4, h - 4, false);

  const int barWidth = POPUP_DEFAULT_MIN_WIDTH;
  const int barHeight = POPUP_DEFAULT_BAR_HEIGHT;
  const int barX = x + (w - barWidth) / 2;
  const int barY = y + renderer.getLineHeight(UI_12_FONT_ID) + margin * 2 - 6;

  const int textX = x + margin + (contentWidth - textWidth) / 2;
  renderer.drawText(UI_12_FONT_ID, textX, y + margin, message, true, EpdFontFamily::BOLD);
  renderer.displayBuffer();
  return {x, y, w, h, barX, barY, barWidth, barHeight};
}

void ScreenComponents::fillPopupProgress(const GfxRenderer& renderer, const PopupLayout& layout, const int progress) {
  int fillWidth = layout.barWidth * progress / 100;
  if (fillWidth < 0) {
    fillWidth = 0;
  } else if (fillWidth > layout.barWidth) {
    fillWidth = layout.barWidth;
  }

  if (fillWidth > 2) {
    renderer.fillRect(layout.barX + 1, layout.barY + 1, fillWidth - 2, layout.barHeight - 2, true);
  }
  renderer.displayBuffer(EInkDisplay::FAST_REFRESH);
}

void ScreenComponents::drawProgressBar(const GfxRenderer& renderer, const int x, const int y, const int width,
                                       const int height, const size_t current, const size_t total) {
  if (total == 0) {
    return;
  }

  // Use 64-bit arithmetic to avoid overflow for large files
  const int percent = static_cast<int>((static_cast<uint64_t>(current) * 100) / total);

  // Draw outline
  renderer.drawRect(x, y, width, height);

  // Draw filled portion
  const int fillWidth = (width - 4) * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(x + 2, y + 2, fillWidth, height - 4);
  }

  // Draw percentage text centered below bar
  const std::string percentText = std::to_string(percent) + "%";
  renderer.drawCenteredText(UI_10_FONT_ID, y + height + 15, percentText.c_str());
}
