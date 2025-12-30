#include "ScreenComponents.h"

#include <GfxRenderer.h>

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
  constexpr int batteryHeight = 10;
  const int x = left;
  const int y = top + 8;

  // Top line
  renderer.drawLine(x, y, x + batteryWidth - 4, y);
  // Bottom line
  renderer.drawLine(x, y + batteryHeight - 1, x + batteryWidth - 4, y + batteryHeight - 1);
  // Left line
  renderer.drawLine(x, y, x, y + batteryHeight - 1);
  // Battery end
  renderer.drawLine(x + batteryWidth - 4, y, x + batteryWidth - 4, y + batteryHeight - 1);
  renderer.drawLine(x + batteryWidth - 3, y + 2, x + batteryWidth - 1, y + 2);
  renderer.drawLine(x + batteryWidth - 3, y + batteryHeight - 3, x + batteryWidth - 1, y + batteryHeight - 3);
  renderer.drawLine(x + batteryWidth - 1, y + 2, x + batteryWidth - 1, y + batteryHeight - 3);

  // The +1 is to round up, so that we always fill at least one pixel
  int filledWidth = percentage * (batteryWidth - 5) / 100 + 1;
  if (filledWidth > batteryWidth - 5) {
    filledWidth = batteryWidth - 5;  // Ensure we don't overflow
  }

  renderer.fillRect(x + 1, y + 1, filledWidth, batteryHeight - 2);
}
