#include "BaseTheme.h"

#include <GfxRenderer.h>

#include <cstdint>
#include <string>

#include "Battery.h"
#include "fontIds.h"

namespace {
constexpr int rowHeight = 30;
constexpr int pageItems = 23;
}  // namespace

void BaseTheme::drawBattery(const GfxRenderer& renderer, Rect rect, const bool showPercentage) {
  // Left aligned battery icon and percentage
  const uint16_t percentage = battery.readPercentage();
  const auto percentageText = showPercentage ? std::to_string(percentage) + "%" : "";
  renderer.drawText(SMALL_FONT_ID, rect.x + 20, rect.y, percentageText.c_str());
  // 1 column on left, 2 columns on right, 5 columns of battery body
  constexpr int batteryWidth = 15;
  constexpr int batteryHeight = 12;
  const int x = rect.x;
  const int y = rect.y + 6;

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

void BaseTheme::drawProgressBar(const GfxRenderer& renderer, Rect rect, const size_t current, const size_t total) {
  if (total == 0) {
    return;
  }

  // Use 64-bit arithmetic to avoid overflow for large files
  const int percent = static_cast<int>((static_cast<uint64_t>(current) * 100) / total);

  // Draw outline
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);

  // Draw filled portion
  const int fillWidth = (rect.width - 4) * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(rect.x + 2, rect.y + 2, fillWidth, rect.height - 4);
  }

  // Draw percentage text centered below bar
  const std::string percentText = std::to_string(percent) + "%";
  renderer.drawCenteredText(UI_10_FONT_ID, rect.y + rect.height + 15, percentText.c_str());
}

void BaseTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle, bool hasIcon,
                         const std::function<std::string(int index)>& rowIcon, bool hasValue,
                         const std::function<std::string(int index)>& rowValue) {
  // Draw selection
  renderer.fillRect(0, rect.y + selectedIndex % pageItems * rowHeight - 2, rect.width - 1, rowHeight);
  // Draw all items
  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int itemY = rect.y + (i % pageItems) * rowHeight;

    // Draw name
    auto itemName = rowTitle(i);
    auto item = renderer.truncatedText(UI_10_FONT_ID, itemName.c_str(),
                                       rect.width - (hasValue ? 100 : 40));  // TODO truncate according to value width?
    renderer.drawText(UI_10_FONT_ID, 20, itemY, item.c_str(), i != selectedIndex);

    if (hasValue) {
      // Draw value
      std::string valueText = rowValue(i);
      const auto textWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());
      renderer.drawText(UI_10_FONT_ID, rect.width - 20 - textWidth, itemY, valueText.c_str(), i != selectedIndex);
    }
  }
}

Rect BaseTheme::getWindowContentFrame(GfxRenderer& renderer) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  return Rect{0, 60, pageWidth, pageHeight - 120};
}

void BaseTheme::drawWindowFrame(GfxRenderer& renderer, Rect rect, bool isPopup, const char* title) {
  if (title) {
    renderer.drawCenteredText(UI_12_FONT_ID, rect.y, title, true, EpdFontFamily::BOLD);
  }
}

void BaseTheme::drawFullscreenWindowFrame(GfxRenderer& renderer, const char* title) {
  BaseTheme::drawWindowFrame(renderer, Rect{0, 15, renderer.getScreenWidth(), renderer.getScreenHeight()}, false,
                             title);
}
