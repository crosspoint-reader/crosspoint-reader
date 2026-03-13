#include "CompactTheme.h"

#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Utf8.h>

#include <cstdint>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/book24.h"
#include "components/icons/cover.h"
#include "components/icons/file24.h"
#include "components/icons/folder.h"
#include "components/icons/folder24.h"
#include "components/icons/hotspot.h"
#include "components/icons/image24.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/text24.h"
#include "components/icons/transfer.h"
#include "components/icons/wifi.h"
#include "fontIds.h"

// Internal constants
namespace {
  constexpr int batteryPercentSpacing = 4;
  constexpr int hPaddingInSelection = 6;
  constexpr int topHintButtonY = 345;
  constexpr int popupMarginX = 16;
  constexpr int popupMarginY = 12;
  constexpr int maxSubtitleWidth = 100;
  constexpr int maxListValueWidth = 200;
  constexpr int mainMenuIconSize = 32;
  constexpr int listIconSize = 24;
  constexpr int mainMenuColumns = 2;
  int coverWidth = 0;

  const uint8_t* iconForName(UIIcon icon, int size) {
    if (size == 24) {
      switch (icon) {
        case UIIcon::Folder:
          return Folder24Icon;
        case UIIcon::Text:
          return Text24Icon;
        case UIIcon::Image:
          return Image24Icon;
        case UIIcon::Book:
          return Book24Icon;
        case UIIcon::File:
          return File24Icon;
        default:
          return nullptr;
      }
    } else if (size == 32) {
      switch (icon) {
        case UIIcon::Folder:
          return FolderIcon;
        case UIIcon::Book:
          return BookIcon;
        case UIIcon::Recent:
          return RecentIcon;
        case UIIcon::Settings:
          return Settings2Icon;
        case UIIcon::Transfer:
          return TransferIcon;
        case UIIcon::Library:
          return LibraryIcon;
        case UIIcon::Wifi:
          return WifiIcon;
        case UIIcon::Hotspot:
          return HotspotIcon;
        default:
          return nullptr;
      }
    }
    return nullptr;
  }
}  // namespace

// draw space 0x0 to 479x799 (not 0x0 to 480x800)
void CompactTheme::drawBatteryIcon(const GfxRenderer& renderer, Rect rect, const uint16_t percentage) const {
  const int x = rect.x;
  const int y = rect.y + 6; // offset from top to align with drawText extra padding: getFontAscenderSize(SMALL_FONT_ID)
  LOG_DBG("THEME-Compact", "Drawing battery icon: x=%u, y=%u, rect.width=%u, rect.height=%u, percentage=%u", x, y, rect.width, rect.height, percentage);
  // drawLine uses absolute coordinates for destination
  // Top line
  renderer.drawLine(x + 1, y, x + rect.width - 3, y);
  // Bottom line
  renderer.drawLine(x + 1, y + rect.height - 1, x + rect.width - 3, y + rect.height - 1);
  // Left line
  renderer.drawLine(x, y + 1, x, y + rect.height - 2);
  // Battery end
  renderer.drawLine(x + rect.width - 2, y + 1, x + rect.width - 2, y + rect.height - 2);
  renderer.drawPixel(x + rect.width - 1, y + 3);
  renderer.drawPixel(x + rect.width - 1, y + rect.height - 4);
  renderer.drawLine(x + rect.width - 1, y + 4, x + rect.width - 1, y + rect.height - 5);
  // fillRect uses relative coordinates for destination
  // Draw bar
  renderer.fillRect(x + 2, y + 2, (rect.width - 4) * percentage / 100, rect.height - 4);
  // low battery indicator
  if (percentage < 10) {
    renderer.drawLine(x, y + rect.height, x + rect.width - 2, y);
  }
}

void CompactTheme::drawBatteryText(const GfxRenderer& renderer, const int x, const int y, const uint16_t percentage, const bool right) const {
  const auto percentageText = std::to_string(percentage) + "%";
  const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, percentageText.c_str());
  const int truex = right ? x - textWidth : x;
  // Clear the area where we're going to draw the text to prevent ghosting
  renderer.fillRect(truex, y, textWidth, renderer.getTextHeight(SMALL_FONT_ID), false);
  // Draw text to the left of the icon
  renderer.drawText(SMALL_FONT_ID, truex, y, percentageText.c_str());
}

void CompactTheme::drawBatteryLeft(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  const uint16_t percentage = powerManager.getBatteryPercentage();

  drawBatteryIcon(renderer, rect, percentage);
  // Left aligned: icon on left, percentage on right (reader mode)
  if (showPercentage) {
    drawBatteryText(renderer, rect.x + batteryPercentSpacing + CompactMetrics::values.batteryWidth, rect.y, percentage);
  }
}

void CompactTheme::drawBatteryRight(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  const uint16_t percentage = powerManager.getBatteryPercentage();

  drawBatteryIcon(renderer, rect, percentage);
  // Right aligned: percentage on left, icon on right (UI headers)
  if (showPercentage) {
    drawBatteryText(renderer, rect.x - batteryPercentSpacing, rect.y, percentage, true);
  }
}

void CompactTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  LOG_DBG("THEME-Compact", "Drawing header: x=%u, y=%u, rect.width=%u, rect.height=%u", rect.x, rect.y, rect.width, rect.height);
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  const bool showBatteryPercentage = SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  // Position icon at right edge, drawBatteryRight will place text to the left
  drawBatteryRight(renderer, Rect{rect.x + rect.width - batteryPercentSpacing - CompactMetrics::values.batteryWidth, rect.y + CompactMetrics::values.topPadding,
    CompactMetrics::values.batteryWidth, CompactMetrics::values.batteryHeight}, showBatteryPercentage);

  int maxTitleWidth = rect.width - CompactMetrics::values.contentSidePadding * 2 - (subtitle != nullptr ? maxSubtitleWidth : 0);

  const char* newTitle = (title) ? title : "CrossPoint";
  const int titleFont = UI_10_FONT_ID;

  LOG_DBG("THEME-Compact", "Title: title=%s, width=%u", newTitle, maxTitleWidth);
  renderer.drawText(titleFont, rect.x + CompactMetrics::values.contentSidePadding, rect.y + CompactMetrics::values.topPadding,
    renderer.truncatedText(titleFont, newTitle, maxTitleWidth, EpdFontFamily::BOLD).c_str(), true, EpdFontFamily::BOLD);
  renderer.drawLine(rect.x, rect.y + rect.height - 3, rect.x + rect.width - 1, rect.y + rect.height - 3, 1, true);

  if (subtitle) {
    const int subTitleFont = SMALL_FONT_ID;
    LOG_DBG("THEME-Compact", "Subtitle: subtitle=%s", subtitle);
    auto truncatedSubtitle = renderer.truncatedText(subTitleFont, subtitle, maxSubtitleWidth, EpdFontFamily::REGULAR);
    int truncatedSubtitleWidth = renderer.getTextWidth(subTitleFont, truncatedSubtitle.c_str());
    renderer.drawCenteredText(subTitleFont, rect.y + CompactMetrics::values.topPadding, truncatedSubtitle.c_str(), true);
  }
}

void CompactTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  LOG_DBG("THEME-Compact", "Drawing subheader: x=%u, y=%u, rect.width=%u, rect.height=%u", rect.x, rect.y, rect.width, rect.height);
  int currentX = rect.x + CompactMetrics::values.contentSidePadding;
  int rightSpace = CompactMetrics::values.contentSidePadding;
  if (rightLabel) {
    LOG_DBG("THEME-Compact", "Right label: rightLabel=%s", rightLabel);
    auto truncatedRightLabel =
        renderer.truncatedText(SMALL_FONT_ID, rightLabel, maxListValueWidth, EpdFontFamily::REGULAR);
    int rightLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedRightLabel.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - CompactMetrics::values.contentSidePadding - rightLabelWidth,
                      rect.y + 7, truncatedRightLabel.c_str());
    rightSpace += rightLabelWidth + hPaddingInSelection;
  }

  auto truncatedLabel = renderer.truncatedText(
      UI_10_FONT_ID, label, rect.width - CompactMetrics::values.contentSidePadding - rightSpace, EpdFontFamily::REGULAR);
  renderer.drawText(UI_10_FONT_ID, currentX, rect.y + 6, truncatedLabel.c_str(), true, EpdFontFamily::REGULAR);

  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

void CompactTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs, bool selected) const {
  int currentX = rect.x + CompactMetrics::values.contentSidePadding;

  if (selected) {
    renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::LightGray);
  }

  for (const auto& tab : tabs) {
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, tab.label, EpdFontFamily::REGULAR);

    if (tab.selected) {
      if (selected) {
        renderer.fillRectDither(currentX, rect.y + 1, textWidth + 2 * hPaddingInSelection, rect.height - 4, Color::Black);
      } else {
        renderer.fillRectDither(currentX, rect.y, textWidth + 2 * hPaddingInSelection, rect.height - 3,
                                Color::LightGray);
        renderer.drawLine(currentX, rect.y + rect.height - 3, currentX + textWidth + 2 * hPaddingInSelection,
                          rect.y + rect.height - 3, 2, true);
      }
    }

    renderer.drawText(UI_10_FONT_ID, currentX + hPaddingInSelection, rect.y + 6, tab.label, !(tab.selected && selected),
                      EpdFontFamily::REGULAR);

    currentX += textWidth + CompactMetrics::values.tabSpacing + 2 * hPaddingInSelection;
  }

  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

void CompactTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
    const std::function<std::string(int index)>& rowTitle, const std::function<std::string(int index)>& rowSubtitle,
    const std::function<UIIcon(int index)>& rowIcon, const std::function<std::string(int index)>& rowValue, bool highlightValue) const {
  int rowHeight =
      (rowSubtitle != nullptr) ? CompactMetrics::values.listWithSubtitleRowHeight : CompactMetrics::values.listRowHeight;
  int pageItems = rect.height / rowHeight;

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    const int scrollAreaHeight = rect.height;

    // Draw scroll bar
    const int scrollBarHeight = (scrollAreaHeight * pageItems) / itemCount;
    const int currentPage = selectedIndex / pageItems;
    const int scrollBarY = rect.y + ((scrollAreaHeight - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - CompactMetrics::values.scrollBarRightOffset;
    renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + scrollAreaHeight, true);
    renderer.fillRect(scrollBarX - CompactMetrics::values.scrollBarWidth, scrollBarY, CompactMetrics::values.scrollBarWidth,
                      scrollBarHeight, true);
  }

  // Draw selection
  int contentWidth =
      rect.width -
      (totalPages > 1 ? (CompactMetrics::values.scrollBarWidth + CompactMetrics::values.scrollBarRightOffset) : 1);
  if (selectedIndex >= 0) {
    renderer.fillRectDither(CompactMetrics::values.contentSidePadding, rect.y + selectedIndex % pageItems * rowHeight,
                             contentWidth - CompactMetrics::values.contentSidePadding * 2, rowHeight, Color::LightGray);
  }

  int textX = rect.x + CompactMetrics::values.contentSidePadding + hPaddingInSelection;
  int textWidth = contentWidth - CompactMetrics::values.contentSidePadding * 2 - hPaddingInSelection * 2;
  int iconSize;
  if (rowIcon != nullptr) {
    iconSize = (rowSubtitle != nullptr) ? mainMenuIconSize : listIconSize;
    textX += iconSize + hPaddingInSelection;
    textWidth -= iconSize + hPaddingInSelection;
  }

  // Draw all items
  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
  int iconY = (rowSubtitle != nullptr) ? 16 : 10;
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int itemY = rect.y + (i % pageItems) * rowHeight;
    int rowTextWidth = textWidth;

    // Draw name
    int valueWidth = 0;
    std::string valueText = "";
    if (rowValue != nullptr) {
      valueText = rowValue(i);
      valueText = renderer.truncatedText(UI_10_FONT_ID, valueText.c_str(), maxListValueWidth);
      valueWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str()) + hPaddingInSelection;
      rowTextWidth -= valueWidth;
    }

    auto itemName = rowTitle(i);
    auto item = renderer.truncatedText(UI_10_FONT_ID, itemName.c_str(), rowTextWidth);
    renderer.drawText(UI_10_FONT_ID, textX, itemY + 7, item.c_str(), true);

    if (rowIcon != nullptr) {
      UIIcon icon = rowIcon(i);
      const uint8_t* iconBitmap = iconForName(icon, iconSize);
      if (iconBitmap != nullptr) {
        renderer.drawIcon(iconBitmap, rect.x + CompactMetrics::values.contentSidePadding + hPaddingInSelection,
                          itemY + iconY, iconSize, iconSize);
      }
    }

    if (rowSubtitle != nullptr) {
      // Draw subtitle
      std::string subtitleText = rowSubtitle(i);
      auto subtitle = renderer.truncatedText(SMALL_FONT_ID, subtitleText.c_str(), rowTextWidth);
      renderer.drawText(SMALL_FONT_ID, textX, itemY + 30, subtitle.c_str(), true);
    }

    // Draw value
    if (!valueText.empty()) {
      if (i == selectedIndex && highlightValue) {
        renderer.fillRect(
            contentWidth - CompactMetrics::values.contentSidePadding - hPaddingInSelection - valueWidth, itemY,
            valueWidth + hPaddingInSelection, rowHeight, true);
      }

      renderer.drawText(UI_10_FONT_ID, rect.x + contentWidth - CompactMetrics::values.contentSidePadding - valueWidth,
                        itemY + 6, valueText.c_str(), !(i == selectedIndex && highlightValue));
    }
  }
}

void CompactTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3, const char* btn4) const {
  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int font = SMALL_FONT_ID;
  constexpr int buttonWidth = 80;
  const int pageHeight = renderer.getScreenHeight() - 1; // getScreenHeight returns 800 but drawable area is 0-799
  constexpr int textPadding = 4; // getTextHeight only returns the base to the top and the bottom is unknown, so 4 is assumed
  const int buttonHeight = renderer.getTextHeight(font) + textPadding;  // Add some padding to text height
  constexpr int buttonPositions[] = {58, 146, 254, 342};
  const char* labels[] = {btn1, btn2, btn3, btn4};
  LOG_DBG("THEME-Compact", "Drawing button hints: pageHeight=%u, buttonHeight=%u, textPadding=%u", pageHeight, buttonHeight, textPadding);

  renderer.fillRect(0, pageHeight - buttonHeight, renderer.getScreenWidth() - 1, buttonHeight, false);
  for (int i = 0; i < 4; i++) {
    const int x = buttonPositions[i];
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      // Draw the filled background and border
      renderer.drawRect(x, pageHeight - buttonHeight, buttonWidth, buttonHeight, 1, true);
      const int textWidth = renderer.getTextWidth(font, labels[i]);
      const int textX = x + (buttonWidth - 1 - textWidth) / 2;
      renderer.drawText(font, textX, pageHeight - buttonHeight, labels[i]);
    }
  }

  renderer.setOrientation(orig_orientation);
}

void CompactTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = CompactMetrics::values.sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 78;                                       // Height on screen (width when rotated)
  // Position for the button group - buttons share a border so they're adjacent

  const char* labels[] = {topBtn, bottomBtn};

  // Draw the shared border for both buttons as one unit
  const int x = screenWidth - buttonWidth;

  // Draw top button outline
  if (topBtn != nullptr && topBtn[0] != '\0') {
    renderer.drawRect(x, topHintButtonY, buttonWidth, buttonHeight, 1, true);
  }

  // Draw bottom button outline
  if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
    renderer.drawRect(x, topHintButtonY + buttonHeight + 5, buttonWidth, buttonHeight, 1, true);
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

void CompactTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks, const int selectorIndex,
    bool& coverRendered, bool& coverBufferStored, bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const int tileWidth = (rect.width - 2 * CompactMetrics::values.contentSidePadding) / 3;
  const int tileHeight = rect.height;
  const int bookTitleHeight = tileHeight - CompactMetrics::values.homeCoverHeight - hPaddingInSelection;
  const int tileY = rect.y;
  const bool hasContinueReading = !recentBooks.empty();

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    if (!coverRendered) {
      for (int i = 0; i < std::min(static_cast<int>(recentBooks.size()), CompactMetrics::values.homeRecentBooksCount); i++) {
        std::string coverPath = recentBooks[i].coverBmpPath;
        bool hasCover = true;
        int tileX = CompactMetrics::values.contentSidePadding + tileWidth * i;
        if (coverPath.empty()) {
          hasCover = false;
        } else {
          const std::string coverBmpPath =
              UITheme::getCoverThumbPath(coverPath, CompactMetrics::values.homeCoverHeight);

          // First time: load cover from SD and render
          FsFile file;
          if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
            Bitmap bitmap(file);
            if (bitmap.parseHeaders() == BmpReaderError::Ok) {
              float coverHeight = static_cast<float>(bitmap.getHeight());
              float coverWidth = static_cast<float>(bitmap.getWidth());
              float ratio = coverWidth / coverHeight;
              const float tileRatio = static_cast<float>(tileWidth - 2 * hPaddingInSelection) /
                                      static_cast<float>(CompactMetrics::values.homeCoverHeight);
              float cropX = 1.0f - (tileRatio / ratio);

              renderer.drawBitmap(bitmap, tileX + hPaddingInSelection, tileY + hPaddingInSelection,
                                  tileWidth - 2 * hPaddingInSelection, CompactMetrics::values.homeCoverHeight,
                                  cropX);
            } else {
              hasCover = false;
            }
            file.close();
          }
        }
        // Draw either way
        renderer.drawRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection, tileWidth - 2 * hPaddingInSelection,
                          CompactMetrics::values.homeCoverHeight, true);

        if (!hasCover) {
          // Render empty cover
          renderer.fillRect(tileX + hPaddingInSelection,
                            tileY + hPaddingInSelection + (CompactMetrics::values.homeCoverHeight / 3),
                            tileWidth - 2 * hPaddingInSelection, 2 * CompactMetrics::values.homeCoverHeight / 3,
                            true);
          renderer.drawIcon(CoverIcon, tileX + hPaddingInSelection + 24, tileY + hPaddingInSelection + 24, 32, 32);
        }
      }

      coverBufferStored = storeCoverBuffer();
      coverRendered = true;
    }

    for (int i = 0; i < std::min(static_cast<int>(recentBooks.size()), CompactMetrics::values.homeRecentBooksCount);
         i++) {
      bool bookSelected = (selectorIndex == i);

      int tileX = CompactMetrics::values.contentSidePadding + tileWidth * i;
      auto title =
          renderer.truncatedText(UI_10_FONT_ID, recentBooks[i].title.c_str(), tileWidth - 2 * hPaddingInSelection);

      if (bookSelected) {
        // Draw selection box
        renderer.fillRectDither(tileX, tileY, tileWidth, hPaddingInSelection, Color::LightGray);
        renderer.fillRectDither(tileX, tileY + hPaddingInSelection, hPaddingInSelection,
                                CompactMetrics::values.homeCoverHeight, Color::LightGray);
        renderer.fillRectDither(tileX + tileWidth - hPaddingInSelection, tileY + hPaddingInSelection,
                                hPaddingInSelection, CompactMetrics::values.homeCoverHeight, Color::LightGray);
        renderer.fillRectDither(tileX, tileY + CompactMetrics::values.homeCoverHeight + hPaddingInSelection,
                                 tileWidth, bookTitleHeight, Color::LightGray);
      }
      renderer.drawText(UI_10_FONT_ID, tileX + hPaddingInSelection,
                        tileY + tileHeight - bookTitleHeight + hPaddingInSelection + 5, title.c_str(), true);
    }
  } else {
    drawEmptyRecents(renderer, rect);
  }
}

void CompactTheme::drawEmptyRecents(const GfxRenderer& renderer, const Rect rect) const {
  constexpr int padding = 48;
  renderer.drawText(UI_12_FONT_ID, rect.x + padding,
                    rect.y + rect.height / 2 - renderer.getLineHeight(UI_12_FONT_ID) - 2, tr(STR_NO_OPEN_BOOK), true,
                    EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, rect.x + padding, rect.y + rect.height / 2 + 2, tr(STR_START_READING), true);
}

void CompactTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
    const std::function<std::string(int index)>& buttonLabel, const std::function<UIIcon(int index)>& rowIcon) const {
  for (int i = 0; i < buttonCount; ++i) {
    int tileWidth = rect.width - CompactMetrics::values.contentSidePadding * 2;
    Rect tileRect = Rect{rect.x + CompactMetrics::values.contentSidePadding,
                         rect.y + i * (CompactMetrics::values.menuRowHeight + CompactMetrics::values.menuSpacing), tileWidth,
                         CompactMetrics::values.menuRowHeight};

    const bool selected = selectedIndex == i;

    if (selected) {
      renderer.fillRectDither(tileRect.x, tileRect.y, tileRect.width, tileRect.height, Color::LightGray);
    }

    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    int textX = tileRect.x + 16;
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int textY = tileRect.y + (CompactMetrics::values.menuRowHeight - lineHeight) / 2;

    if (rowIcon != nullptr) {
      UIIcon icon = rowIcon(i);
      const uint8_t* iconBitmap = iconForName(icon, mainMenuIconSize);
      if (iconBitmap != nullptr) {
        renderer.drawIcon(iconBitmap, textX, textY + 3, mainMenuIconSize, mainMenuIconSize);
        textX += mainMenuIconSize + hPaddingInSelection + 2;
      }
    }

    renderer.drawText(UI_12_FONT_ID, textX, textY, label, true);
  }
}

Rect CompactTheme::drawPopup(const GfxRenderer& renderer, const char* message) const {
  constexpr int y = 132;
  constexpr int outline = 2;
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, message, EpdFontFamily::REGULAR);
  const int textHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int w = textWidth + popupMarginX * 2;
  const int h = textHeight + popupMarginY * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;

  renderer.fillRectDither(x - outline, y - outline, w + outline * 2, h + outline * 2, Color::White);
  renderer.fillRectDither(x, y, w, h, Color::Black);

  const int textX = x + (w - textWidth) / 2;
  const int textY = y + popupMarginY - 2;
  renderer.drawText(UI_12_FONT_ID, textX, textY, message, false, EpdFontFamily::REGULAR);
  renderer.displayBuffer();

  return Rect{x, y, w, h};
}

void CompactTheme::fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const {
  constexpr int barHeight = 4;

  // Twice the margin in drawPopup to match text width
  const int barWidth = layout.width - popupMarginX * 2;
  const int barX = layout.x + (layout.width - barWidth) / 2;
  // Center inside the margin of drawPopup. The - 1 is added to account for the - 2 in drawPopup.
  const int barY = layout.y + layout.height - popupMarginY / 2 - barHeight / 2 - 1;

  int fillWidth = barWidth * progress / 100;

  renderer.fillRect(barX, barY, fillWidth, barHeight, false);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void CompactTheme::drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth) const {
  int lineY = rect.y + rect.height + renderer.getLineHeight(UI_12_FONT_ID) + CompactMetrics::values.verticalSpacing;
  int lineW = textWidth + hPaddingInSelection * 2;
  renderer.drawLine(rect.x + (rect.width - lineW) / 2, lineY, rect.x + (rect.width + lineW) / 2, lineY, 3);
}

void CompactTheme::drawKeyboardKey(const GfxRenderer& renderer, Rect rect, const char* label, const bool isSelected) const {
  if (isSelected) {
    renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::Black);
  }

  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, label);
  const int textX = rect.x + (rect.width - textWidth) / 2;
  const int textY = rect.y + (rect.height - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
  renderer.drawText(UI_12_FONT_ID, textX, textY, label, !isSelected);
}

