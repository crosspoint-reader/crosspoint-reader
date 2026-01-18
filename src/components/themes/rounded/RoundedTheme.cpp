#include "RoundedTheme.h"

#include <GfxRenderer.h>

#include <cstdint>
#include <string>

#include "Battery.h"
#include "fontIds.h"

namespace {
constexpr int rowHeight = 64;
constexpr int pageItems = 9;
constexpr int windowCornerRadius = 16;
constexpr int windowBorderWidth = 2;
constexpr int fullscreenWindowMargin = 20;
constexpr int windowHeaderHeight = 50;
constexpr int statusBarHeight = 50;
constexpr int buttonHintsHeight = 50;
}  // namespace

void RoundedTheme::drawBattery(const GfxRenderer& renderer, Rect rect, const bool showPercentage) {
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

  // Draw bars
  if (percentage > 10) {
    renderer.fillRect(x + 2, y + 2, 3, batteryHeight - 4);
  }
  if (percentage > 40) {
    renderer.fillRect(x + 6, y + 2, 3, batteryHeight - 4);
  }
  if (percentage > 70) {
    renderer.fillRect(x + 10, y + 2, 2, batteryHeight - 4);
  }
}

void RoundedTheme::drawProgressBar(const GfxRenderer& renderer, Rect rect, const size_t current, const size_t total) {
  if (total == 0) {
    return;
  }

  // Use 64-bit arithmetic to avoid overflow for large files
  const int percent = static_cast<int>((static_cast<uint64_t>(current) * 100) / total);

  // Draw filled portion
  const int fillWidth = (rect.width - 4) * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(rect.x + 2, rect.y + 2, fillWidth, rect.height - 4);
  }

  // Draw percentage text centered below bar
  const std::string percentText = std::to_string(percent) + "%";
  renderer.drawCenteredText(UI_10_FONT_ID, rect.y + rect.height + 15, percentText.c_str());
}

void RoundedTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                            const std::function<std::string(int index)>& rowTitle, bool hasIcon,
                            const std::function<std::string(int index)>& rowIcon, bool hasValue,
                            const std::function<std::string(int index)>& rowValue) {
  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    // Draw scroll bar
    const int scrollBarHeight = (rect.height * pageItems) / itemCount;
    const int currentPage = selectedIndex / pageItems;
    const int scrollBarY = rect.y + ((rect.height - scrollBarHeight) * currentPage) / (totalPages - 1);
    renderer.fillRectGrey(rect.x + rect.width, rect.y, 4, rect.height, 5);
    renderer.fillRect(rect.x + rect.width, scrollBarY, 4, scrollBarHeight, true);
  }

  // Draw selection
  renderer.fillRectGrey(rect.x, rect.y + selectedIndex % pageItems * rowHeight - 2, rect.width - 1, rowHeight, 3);
  // Draw all items
  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int itemY = rect.y + (i % pageItems) * rowHeight;

    // Draw name
    auto itemName = rowTitle(i);
    auto item = renderer.truncatedText(UI_10_FONT_ID, itemName.c_str(),
                                       rect.width - (hasValue ? 100 : 40));  // TODO truncate according to value width?
    renderer.drawText(UI_10_FONT_ID, rect.x + 20, itemY + 20, item.c_str(), true);

    if (hasValue) {
      // Draw value
      std::string valueText = rowValue(i);
      const auto textWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());
      renderer.drawText(UI_10_FONT_ID, rect.x + rect.width - 20 - textWidth, itemY + 20, valueText.c_str(), true);
    }
  }
}

Rect RoundedTheme::getWindowContentFrame(GfxRenderer& renderer) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  return Rect{35, 125, pageWidth - 70, pageHeight - 165 - buttonHintsHeight};
}

void RoundedTheme::drawWindowFrame(GfxRenderer& renderer, Rect rect, bool isPopup, const char* title) {
  const int windowWidth = renderer.getScreenWidth() - 2 * rect.x;

  if (title) {  // Header background
    renderer.fillRectGrey(rect.x, rect.y, windowWidth, windowHeaderHeight, 5);
    renderer.fillArc(windowCornerRadius, rect.x + windowCornerRadius, rect.y + windowCornerRadius, -1, -1, 0,
                     -1);  // TL
    renderer.fillArc(windowCornerRadius, windowWidth + rect.x - windowCornerRadius, rect.y + windowCornerRadius, 1, -1,
                     0, -1);  // TR
  }

  renderer.drawRoundedRect(rect.x, rect.y, windowWidth, rect.height, windowBorderWidth, windowCornerRadius, true);

  if (!isPopup) {
    renderer.drawLine(windowWidth + rect.x, rect.y + windowCornerRadius + 2, windowWidth + rect.x,
                      rect.y + rect.height - windowCornerRadius, windowBorderWidth, true);
    renderer.drawLine(rect.x + windowCornerRadius + 2, rect.y + rect.height, windowWidth + rect.x - windowCornerRadius,
                      rect.y + rect.height, windowBorderWidth, true);
    renderer.drawArc(windowCornerRadius + windowBorderWidth, windowWidth + rect.x - 1 - windowCornerRadius,
                     rect.y + rect.height - 1 - windowCornerRadius, 1, 1, windowBorderWidth, true);
    renderer.drawPixel(rect.x + windowCornerRadius + 1, rect.y + rect.height, true);
  }

  if (title) {  // Header
    const int titleWidth = renderer.getTextWidth(UI_12_FONT_ID, title);
    const int titleX = (renderer.getScreenWidth() - titleWidth) / 2;
    const int titleY = rect.y + 10;
    renderer.drawText(UI_12_FONT_ID, titleX, titleY, title, true, EpdFontFamily::REGULAR);
    renderer.drawLine(rect.x, rect.y + windowHeaderHeight, windowWidth + rect.x, rect.y + windowHeaderHeight,
                      windowBorderWidth, true);
  }
}

void RoundedTheme::drawFullscreenWindowFrame(GfxRenderer& renderer, const char* title) {
  // drawStatusBar(renderer);
  RoundedTheme::drawWindowFrame(
      renderer,
      Rect{fullscreenWindowMargin, statusBarHeight, 0,
           renderer.getScreenHeight() - fullscreenWindowMargin - statusBarHeight - buttonHintsHeight},
      false, title);
  RoundedTheme::drawBattery(renderer, Rect{fullscreenWindowMargin, 18}, false);
}
