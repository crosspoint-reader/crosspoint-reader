#include "LyraTheme.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>

#include <cstdint>
#include <string>

#include "Battery.h"
#include "RecentBooksStore.h"
#include "fontIds.h"
#include "util/StringUtils.h"

// Internal constants
namespace {
constexpr int batteryPercentSpacing = 4;
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;
constexpr int topHintButtonY = 345;
}  // namespace

void LyraTheme::drawBattery(const GfxRenderer& renderer, Rect rect, const bool showPercentage) {
  // Left aligned battery icon and percentage
  const uint16_t percentage = battery.readPercentage();
  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    renderer.drawText(SMALL_FONT_ID, rect.x + batteryPercentSpacing + LyraMetrics::values.batteryWidth, rect.y,
                      percentageText.c_str());
  }
  // 1 column on left, 2 columns on right, 5 columns of battery body
  const int x = rect.x;
  const int y = rect.y + 6;
  const int battWidth = LyraMetrics::values.batteryWidth;

  // Top line
  renderer.drawLine(x + 1, y, x + battWidth - 3, y);
  // Bottom line
  renderer.drawLine(x + 1, y + rect.height - 1, x + battWidth - 3, y + rect.height - 1);
  // Left line
  renderer.drawLine(x, y + 1, x, y + rect.height - 2);
  // Battery end
  renderer.drawLine(x + battWidth - 2, y + 1, x + battWidth - 2, y + rect.height - 2);
  renderer.drawPixel(x + battWidth - 1, y + 3);
  renderer.drawPixel(x + battWidth - 1, y + rect.height - 4);
  renderer.drawLine(x + battWidth - 0, y + 4, x + battWidth - 0, y + rect.height - 5);

  // Draw bars
  if (percentage > 10) {
    renderer.fillRect(x + 2, y + 2, 3, rect.height - 4);
  }
  if (percentage > 40) {
    renderer.fillRect(x + 6, y + 2, 3, rect.height - 4);
  }
  if (percentage > 70) {
    renderer.fillRect(x + 10, y + 2, 3, rect.height - 4);
  }
}

void LyraTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title) {
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  int batteryX = rect.x + rect.width - LyraMetrics::values.contentSidePadding - LyraMetrics::values.batteryWidth;
  if (showBatteryPercentage) {
    const uint16_t percentage = battery.readPercentage();
    const auto percentageText = std::to_string(percentage) + "%";
    batteryX -= renderer.getTextWidth(SMALL_FONT_ID, percentageText.c_str());
  }
  drawBattery(renderer,
              Rect{batteryX, rect.y + 10, LyraMetrics::values.batteryWidth, LyraMetrics::values.batteryHeight},
              showBatteryPercentage);

  if (title) {
    renderer.drawText(UI_12_FONT_ID, rect.x + LyraMetrics::values.contentSidePadding,
                      rect.y + LyraMetrics::values.batteryBarHeight + 3, title, true, EpdFontFamily::BOLD);
    renderer.drawLine(rect.x, rect.y + rect.height - 3, rect.x + rect.width, rect.y + rect.height - 3, 3, true);
  }
}

void LyraTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs) {
  int currentX = rect.x + LyraMetrics::values.contentSidePadding;

  for (const auto& tab : tabs) {
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, tab.label, EpdFontFamily::REGULAR);

    // Draw tab label
    renderer.drawText(UI_10_FONT_ID, currentX, rect.y + 6, tab.label, true, EpdFontFamily::REGULAR);

    // Draw underline for selected tab
    if (tab.selected) {
      renderer.drawLine(currentX, rect.y + rect.height - 2, currentX + textWidth, rect.y + rect.height - 2, true);
    }

    currentX += textWidth + LyraMetrics::values.tabSpacing;
  }

  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width, rect.y + rect.height - 1, true);
}

void LyraTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle, bool hasIcon,
                         const std::function<std::string(int index)>& rowIcon, bool hasValue,
                         const std::function<std::string(int index)>& rowValue) {
  int pageItems = rect.height / LyraMetrics::values.listRowHeight;
  const int rowHeight = LyraMetrics::values.listRowHeight;

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    const int scrollAreaHeight = topHintButtonY - rect.y - LyraMetrics::values.verticalSpacing;

    // Draw scroll bar
    const int scrollBarHeight = (scrollAreaHeight * pageItems) / itemCount;
    const int currentPage = selectedIndex / pageItems;
    const int scrollBarY = rect.y + ((scrollAreaHeight - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - LyraMetrics::values.scrollBarRightOffset;
    renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + scrollAreaHeight, true);
    renderer.fillRect(scrollBarX - LyraMetrics::values.scrollBarWidth, scrollBarY, LyraMetrics::values.scrollBarWidth,
                      scrollBarHeight, true);
  }

  // Draw selection
  int contentWidth =
      rect.width -
      (totalPages > 1 ? (LyraMetrics::values.scrollBarWidth + LyraMetrics::values.scrollBarRightOffset) : 1);
  renderer.fillRoundedRect(LyraMetrics::values.contentSidePadding, rect.y + selectedIndex % pageItems * rowHeight,
                           contentWidth - LyraMetrics::values.contentSidePadding * 2, rowHeight, cornerRadius,
                           COLOR_LIGHT_GRAY);

  // Draw all items
  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int itemY = rect.y + (i % pageItems) * rowHeight;

    // Draw name
    auto itemName = rowTitle(i);
    auto item =
        renderer.truncatedText(UI_10_FONT_ID, itemName.c_str(),
                               contentWidth - LyraMetrics::values.contentSidePadding * 2 - hPaddingInSelection * 2 -
                                   (hasValue ? 60 : 0));  // TODO truncate according to value width?
    renderer.drawText(UI_10_FONT_ID, rect.x + LyraMetrics::values.contentSidePadding + hPaddingInSelection * 2,
                      itemY + 6, item.c_str(), true);

    if (hasValue) {
      // Draw value
      std::string valueText = rowValue(i);
      if (!valueText.empty()) {
        const auto textWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());

        if (i == selectedIndex) {
          renderer.fillRoundedRect(
              contentWidth - LyraMetrics::values.contentSidePadding - hPaddingInSelection * 2 - textWidth, itemY,
              textWidth + hPaddingInSelection * 2, rowHeight, cornerRadius, COLOR_BLACK);
        }

        renderer.drawText(UI_10_FONT_ID,
                          contentWidth - LyraMetrics::values.contentSidePadding - hPaddingInSelection - textWidth,
                          itemY + 6, valueText.c_str(), i != selectedIndex);
      }
    }
  }
}

void LyraTheme::drawButtonHints(const GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) {
  // TODO: Fix rotated hints
  // const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  // renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = 80;
  constexpr int buttonHeight = LyraMetrics::values.buttonHintsHeight;
  constexpr int buttonY = LyraMetrics::values.buttonHintsHeight;  // Distance from bottom
  constexpr int textYOffset = 7;                                  // Distance from top of button to text baseline
  constexpr int buttonPositions[] = {68, 156, 244, 332};
  const char* labels[] = {btn1, btn2, btn3, btn4};

  for (int i = 0; i < 4; i++) {
    // Only draw if the label is non-empty
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int x = buttonPositions[i];
      renderer.fillRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, false);
      renderer.drawRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, 1, cornerRadius, true, true, false,
                               false, true);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
      const int textX = x + (buttonWidth - 1 - textWidth) / 2;
      renderer.drawText(SMALL_FONT_ID, textX, pageHeight - buttonY + textYOffset, labels[i]);
    }
  }

  // renderer.setOrientation(orig_orientation);
}

void LyraTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) {
  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = LyraMetrics::values.sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 80;                                       // Height on screen (width when rotated)
  // Position for the button group - buttons share a border so they're adjacent

  const char* labels[] = {topBtn, bottomBtn};

  // Draw the shared border for both buttons as one unit
  const int x = screenWidth - buttonWidth;

  // Draw top button outline
  if (topBtn != nullptr && topBtn[0] != '\0') {
    renderer.drawRoundedRect(x, topHintButtonY, buttonWidth, buttonHeight, 1, cornerRadius, true, false, true, false,
                             true);
  }

  // Draw bottom button outline
  if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
    renderer.drawRoundedRect(x, topHintButtonY + buttonHeight + 5, buttonWidth, buttonHeight, 1, cornerRadius, true,
                             false, true, false, true);
  }

  // Draw text for each button
  for (int i = 0; i < 2; i++) {
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int y = topHintButtonY + (i * buttonHeight + 5);

      // Draw rotated text centered in the button
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);

      renderer.drawTextRotated90CW(SMALL_FONT_ID, x, y + (buttonHeight + textWidth) / 2, labels[i]);
    }
  }
}

void LyraTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect,
                                    const std::vector<RecentBookWithCover>& recentBooks, const int selectorIndex,
                                    bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                                    std::function<bool()> storeCoverBuffer) {
  const int tileWidth = (rect.width - 2 * LyraMetrics::values.contentSidePadding) / 3;
  const int tileHeight = rect.height;
  const int bookTitleHeight = 53;
  const int tileY = rect.y;
  const bool hasContinueReading = !recentBooks.empty();

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    if (!coverRendered) {
      for (int i = 0; i < std::min(static_cast<int>(recentBooks.size()), LyraMetrics::values.homeRecentBooksCount);
           i++) {
        const std::string& coverBmpPath = recentBooks[i].coverBmpPath;

        if (coverBmpPath.empty()) {
          continue;
        }

        int tileX = LyraMetrics::values.contentSidePadding + tileWidth * i;

        // First time: load cover from SD and render
        FsFile file;
        if (SdMan.openFileForRead("HOME", coverBmpPath, file)) {
          Bitmap bitmap(file);
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            renderer.drawBitmap(bitmap, tileX + hPaddingInSelection, tileY + hPaddingInSelection,
                                tileWidth - 2 * hPaddingInSelection,
                                tileHeight - bookTitleHeight - hPaddingInSelection);
          }
          file.close();
        }
      }

      coverBufferStored = storeCoverBuffer();
      coverRendered = true;
    }

    for (int i = 0; i < std::min(static_cast<int>(recentBooks.size()), LyraMetrics::values.homeRecentBooksCount); i++) {
      bool bookSelected = (selectorIndex == i);

      int tileX = LyraMetrics::values.contentSidePadding + tileWidth * i;
      auto title =
          renderer.truncatedText(UI_10_FONT_ID, recentBooks[i].book.title.c_str(), tileWidth - 2 * hPaddingInSelection);

      if (bookSelected) {
        // Draw selection box
        renderer.fillRoundedRect(tileX, tileY, tileWidth, hPaddingInSelection, cornerRadius, true, true, false, false,
                                 COLOR_LIGHT_GRAY);
        renderer.fillRectDither(tileX, tileY + hPaddingInSelection, hPaddingInSelection,
                                tileHeight - hPaddingInSelection, COLOR_LIGHT_GRAY);
        renderer.fillRectDither(tileX + tileWidth - hPaddingInSelection, tileY + hPaddingInSelection,
                                hPaddingInSelection, tileHeight - hPaddingInSelection, COLOR_LIGHT_GRAY);
        renderer.fillRoundedRect(tileX, tileY + tileHeight + hPaddingInSelection - bookTitleHeight, tileWidth,
                                 bookTitleHeight, cornerRadius, false, false, true, true, COLOR_LIGHT_GRAY);
      }
      renderer.drawText(UI_10_FONT_ID, tileX + hPaddingInSelection,
                        tileY + tileHeight - bookTitleHeight + hPaddingInSelection + 5, title.c_str(), true);
    }
  }
}

void LyraTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel, bool hasIcon,
                               const std::function<std::string(int index)>& rowIcon) {
  for (int i = 0; i < buttonCount; ++i) {
    int tileWidth = (rect.width - LyraMetrics::values.contentSidePadding * 2 - LyraMetrics::values.menuSpacing) / 2;
    Rect tileRect =
        Rect{rect.x + LyraMetrics::values.contentSidePadding + (LyraMetrics::values.menuSpacing + tileWidth) * (i % 2),
             rect.y + static_cast<int>(i / 2) * (LyraMetrics::values.menuRowHeight + LyraMetrics::values.menuSpacing),
             tileWidth, LyraMetrics::values.menuRowHeight};

    const bool selected = selectedIndex == i;

    if (selected) {
      renderer.fillRoundedRect(tileRect.x, tileRect.y, tileRect.width, tileRect.height, cornerRadius, COLOR_LIGHT_GRAY);
    }

    const char* label = buttonLabel(i).c_str();
    const int textX = tileRect.x + 16;
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int textY = tileRect.y + (LyraMetrics::values.menuRowHeight - lineHeight) / 2;

    // Invert text when the tile is selected, to contrast with the filled background
    renderer.drawText(UI_12_FONT_ID, textX, textY, label, true);
  }
}

void LyraTheme::drawPopup(GfxRenderer& renderer, const char* message) {
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, message, EpdFontFamily::REGULAR);
  constexpr int margin = 20;
  const int x = (renderer.getScreenWidth() - textWidth - margin * 2) / 2;
  constexpr int y = 117;
  const int w = textWidth + margin * 2;
  const int h = renderer.getLineHeight(UI_12_FONT_ID) + margin * 2;

  renderer.fillRect(x - 5, y - 5, w + 10, h + 10, false);
  renderer.drawRect(x + 5, y + 5, w - 10, h - 10, true);
  renderer.drawText(UI_12_FONT_ID, x + margin, y + margin, message, true, EpdFontFamily::REGULAR);
  renderer.displayBuffer();
}