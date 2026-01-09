#include "ScreenComponents.h"

#include <GfxRenderer.h>

#include <cstdint>
#include <string>

#include "Battery.h"
#include "fontIds.h"

void ScreenComponents::drawBattery(const GfxRenderer& renderer, const int left, const int top) {
  // Left aligned battery icon and percentage
  const uint16_t percentage = battery.readPercentage();
  const auto percentageText = std::to_string(percentage) + "%";
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
