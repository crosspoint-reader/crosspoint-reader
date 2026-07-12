#include "NoirTheme.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <I18n.h>

#include <cstdint>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
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

namespace {
constexpr int batteryPercentSpacing = 4;
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;
constexpr int topHintButtonY = 345;
constexpr int maxSubtitleWidth = 100;
constexpr int maxListValueWidth = 200;
constexpr int mainMenuIconSize = 32;
constexpr int listIconSize = 24;

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

void NoirTheme::drawBatteryRight(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  if (!showPercentage) return;
  const uint16_t percentage = powerManager.getBatteryPercentage();
  const auto percentageText = std::to_string(percentage) + "%";
  const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, percentageText.c_str());
  // White text on black header background
  renderer.drawText(SMALL_FONT_ID, rect.x - textWidth, rect.y, percentageText.c_str(), false);
}

void NoirTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  // Fill entire header area with black
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, true);

  // Battery percentage in white (right side)
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const int batteryX = rect.x + rect.width - 12 - NoirMetrics::values.batteryWidth;
  drawBatteryRight(renderer,
                   Rect{batteryX, rect.y + 5, NoirMetrics::values.batteryWidth, NoirMetrics::values.batteryHeight},
                   showBatteryPercentage);

  int maxTitleWidth =
      rect.width - NoirMetrics::values.contentSidePadding * 2 - (subtitle != nullptr ? maxSubtitleWidth : 0);

  if (title) {
    auto truncatedTitle = renderer.truncatedText(UI_12_FONT_ID, title, maxTitleWidth, EpdFontFamily::BOLD);
    // White bold text on black background
    renderer.drawText(UI_12_FONT_ID, rect.x + NoirMetrics::values.contentSidePadding,
                      rect.y + NoirMetrics::values.batteryBarHeight + 1, truncatedTitle.c_str(), false,
                      EpdFontFamily::BOLD);
  }

  if (subtitle) {
    auto truncatedSubtitle = renderer.truncatedText(SMALL_FONT_ID, subtitle, maxSubtitleWidth, EpdFontFamily::REGULAR);
    int truncatedSubtitleWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedSubtitle.c_str());
    // White subtitle text
    renderer.drawText(SMALL_FONT_ID,
                      rect.x + rect.width - NoirMetrics::values.contentSidePadding - truncatedSubtitleWidth,
                      rect.y + 42, truncatedSubtitle.c_str(), false);
  }
}

void NoirTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  // Black background
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, true);

  int currentX = rect.x + NoirMetrics::values.contentSidePadding;
  int rightSpace = NoirMetrics::values.contentSidePadding;

  if (rightLabel) {
    auto truncatedRightLabel = renderer.truncatedText(SMALL_FONT_ID, rightLabel, maxListValueWidth, EpdFontFamily::REGULAR);
    int rightLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedRightLabel.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - NoirMetrics::values.contentSidePadding - rightLabelWidth,
                      rect.y + 7, truncatedRightLabel.c_str(), false);  // white text
    rightSpace += rightLabelWidth + hPaddingInSelection;
  }

  auto truncatedLabel = renderer.truncatedText(
      UI_10_FONT_ID, label, rect.width - NoirMetrics::values.contentSidePadding - rightSpace, EpdFontFamily::REGULAR);
  renderer.drawText(UI_10_FONT_ID, currentX, rect.y + 6, truncatedLabel.c_str(), false, EpdFontFamily::REGULAR);  // white text
}

void NoirTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                           bool selected) const {
  int currentX = rect.x + NoirMetrics::values.contentSidePadding;

  for (const auto& tab : tabs) {
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, tab.label, EpdFontFamily::REGULAR);
    const int tabWidth = textWidth + 2 * hPaddingInSelection;

    if (tab.selected) {
      if (selected) {
        // Active + focused: full black fill with white text
        renderer.fillRoundedRect(currentX, rect.y + 1, tabWidth, rect.height - 4, cornerRadius, Color::Black);
      } else {
        // Active but not focused: black fill, slightly inset
        renderer.fillRoundedRect(currentX, rect.y + 2, tabWidth, rect.height - 5, cornerRadius, Color::Black);
      }
    }

    // White text if this tab is selected, black text otherwise
    renderer.drawText(UI_10_FONT_ID, currentX + hPaddingInSelection, rect.y + 6, tab.label,
                      !tab.selected, EpdFontFamily::REGULAR);

    currentX += tabWidth + NoirMetrics::values.tabSpacing;
  }

  // Bottom separator line
  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

void NoirTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<UIIcon(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue, bool highlightValue) const {
  int rowHeight =
      (rowSubtitle != nullptr) ? NoirMetrics::values.listWithSubtitleRowHeight : NoirMetrics::values.listRowHeight;
  int pageItems = rect.height / rowHeight;

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    const int scrollAreaHeight = rect.height;
    const int scrollBarHeight = (scrollAreaHeight * pageItems) / itemCount;
    const int currentPage = selectedIndex / pageItems;
    const int scrollBarY = rect.y + ((scrollAreaHeight - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - NoirMetrics::values.scrollBarRightOffset;
    renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + scrollAreaHeight, true);
    renderer.fillRect(scrollBarX - NoirMetrics::values.scrollBarWidth, scrollBarY, NoirMetrics::values.scrollBarWidth,
                      scrollBarHeight, true);
  }

  int contentWidth =
      rect.width -
      (totalPages > 1 ? (NoirMetrics::values.scrollBarWidth + NoirMetrics::values.scrollBarRightOffset) : 1);

  // Draw selection — BLACK fill instead of gray
  if (selectedIndex >= 0) {
    renderer.fillRoundedRect(NoirMetrics::values.contentSidePadding, rect.y + selectedIndex % pageItems * rowHeight,
                             contentWidth - NoirMetrics::values.contentSidePadding * 2, rowHeight, cornerRadius,
                             Color::Black);
  }

  int textX = rect.x + NoirMetrics::values.contentSidePadding + hPaddingInSelection;
  int textWidth = contentWidth - NoirMetrics::values.contentSidePadding * 2 - hPaddingInSelection * 2;
  int iconSize = 0;
  if (rowIcon != nullptr) {
    iconSize = (rowSubtitle != nullptr) ? mainMenuIconSize : listIconSize;
    textX += iconSize + hPaddingInSelection;
    textWidth -= iconSize + hPaddingInSelection;
  }

  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
  int iconY = (rowSubtitle != nullptr) ? 16 : 10;
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int itemY = rect.y + (i % pageItems) * rowHeight;
    const bool isSelected = (i == selectedIndex);
    int rowTextWidth = textWidth;

    // Draw value (right side)
    int valueWidth = 0;
    std::string valueText = "";
    if (rowValue != nullptr) {
      valueText = rowValue(i);
      valueText = renderer.truncatedText(UI_10_FONT_ID, valueText.c_str(), maxListValueWidth);
      valueWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str()) + hPaddingInSelection;
      rowTextWidth -= valueWidth;
    }

    // Draw title — white text if selected, black if not
    auto itemName = rowTitle(i);
    auto item = renderer.truncatedText(UI_10_FONT_ID, itemName.c_str(), rowTextWidth);
    renderer.drawText(UI_10_FONT_ID, textX, itemY + 7, item.c_str(), !isSelected);

    // Draw icon
    if (rowIcon != nullptr) {
      UIIcon icon = rowIcon(i);
      const uint8_t* iconBitmap = iconForName(icon, iconSize);
      if (iconBitmap != nullptr) {
        int iconX = rect.x + NoirMetrics::values.contentSidePadding + hPaddingInSelection;
        if (isSelected) {
          // Draw white background behind icon so it's visible on black selection
          renderer.fillRect(iconX - 1, itemY + iconY - 1, iconSize + 2, iconSize + 2, false);
        }
        renderer.drawIcon(iconBitmap, iconX, itemY + iconY, iconSize, iconSize);
      }
    }

    // Draw subtitle
    if (rowSubtitle != nullptr) {
      std::string subtitleText = rowSubtitle(i);
      auto subtitle = renderer.truncatedText(SMALL_FONT_ID, subtitleText.c_str(), rowTextWidth);
      renderer.drawText(SMALL_FONT_ID, textX, itemY + 30, subtitle.c_str(), !isSelected);
    }

    // Draw value
    if (!valueText.empty()) {
      if (isSelected && highlightValue) {
        // White pill on black selection for editable value
        renderer.fillRoundedRect(
            contentWidth - NoirMetrics::values.contentSidePadding - hPaddingInSelection - valueWidth, itemY,
            valueWidth + hPaddingInSelection, rowHeight, cornerRadius, Color::White);
        renderer.drawText(UI_10_FONT_ID, rect.x + contentWidth - NoirMetrics::values.contentSidePadding - valueWidth,
                          itemY + 6, valueText.c_str(), true);  // black text on white pill
      } else {
        renderer.drawText(UI_10_FONT_ID, rect.x + contentWidth - NoirMetrics::values.contentSidePadding - valueWidth,
                          itemY + 6, valueText.c_str(), !isSelected);
      }
    }
  }
}

void NoirTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  for (int i = 0; i < buttonCount; ++i) {
    int tileWidth = rect.width - NoirMetrics::values.contentSidePadding * 2;
    Rect tileRect = Rect{rect.x + NoirMetrics::values.contentSidePadding,
                         rect.y + i * (NoirMetrics::values.menuRowHeight + NoirMetrics::values.menuSpacing), tileWidth,
                         NoirMetrics::values.menuRowHeight};

    const bool selected = selectedIndex == i;

    if (selected) {
      // Selected: black fill
      renderer.fillRoundedRect(tileRect.x, tileRect.y, tileRect.width, tileRect.height, cornerRadius, Color::Black);
    } else {
      // Not selected: thin rounded border only
      renderer.drawRoundedRect(tileRect.x, tileRect.y, tileRect.width, tileRect.height, 1, cornerRadius, true);
    }

    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    int textX = tileRect.x + 16;
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int textY = tileRect.y + (NoirMetrics::values.menuRowHeight - lineHeight) / 2;

    if (rowIcon != nullptr) {
      UIIcon icon = rowIcon(i);
      const uint8_t* iconBitmap = iconForName(icon, mainMenuIconSize);
      if (iconBitmap != nullptr) {
        if (selected) {
          // White background behind icon so it's visible on black
          renderer.fillRect(textX - 1, textY + 2, mainMenuIconSize + 2, mainMenuIconSize + 2, false);
        }
        renderer.drawIcon(iconBitmap, textX, textY + 3, mainMenuIconSize, mainMenuIconSize);
        textX += mainMenuIconSize + hPaddingInSelection + 2;
      }
    }

    // White text if selected, black text if not
    renderer.drawText(UI_12_FONT_ID, textX, textY, label, !selected);
  }
}

void NoirTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) const {
  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = 80;
  constexpr int smallButtonHeight = 15;
  constexpr int buttonHeight = NoirMetrics::values.buttonHintsHeight;
  constexpr int buttonY = NoirMetrics::values.buttonHintsHeight;
  constexpr int textYOffset = 7;
  constexpr int buttonPositions[] = {58, 146, 254, 342};
  const char* labels[] = {btn1, btn2, btn3, btn4};

  for (int i = 0; i < 4; i++) {
    const int x = buttonPositions[i];
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      // Black filled rounded button with white text
      renderer.fillRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, cornerRadius, Color::Black);
      if (!BaseTheme::drawArrowIfNeeded(renderer, labels[i], x + buttonWidth / 2, pageHeight - buttonY + buttonHeight / 2, 5, false)) {
        const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
        const int textX = x + (buttonWidth - 1 - textWidth) / 2;
        renderer.drawText(SMALL_FONT_ID, textX, pageHeight - buttonY + textYOffset, labels[i], false);  // white text
      }
    } else {
      // Empty button: just a thin outline
      renderer.drawRoundedRect(x, pageHeight - smallButtonHeight, buttonWidth, smallButtonHeight, 1, cornerRadius, true);
    }
  }

  renderer.setOrientation(orig_orientation);
}

void NoirTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = NoirMetrics::values.sideButtonHintsWidth;
  constexpr int buttonHeight = 78;
  const char* labels[] = {topBtn, bottomBtn};
  const int x = screenWidth - buttonWidth;

  for (int i = 0; i < 2; i++) {
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int y = topHintButtonY + i * (buttonHeight + 5);
      // Black filled rounded button
      renderer.fillRoundedRect(x, y, buttonWidth, buttonHeight, cornerRadius,
                               true, false, true, false, Color::Black);
      if (!BaseTheme::drawArrowIfNeeded(renderer, labels[i], x + buttonWidth / 2, y + buttonHeight / 2, 5, false)) {
        // White rotated text
        const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
        renderer.drawTextRotated90CW(SMALL_FONT_ID, x, y + (buttonHeight + textWidth) / 2, labels[i], false);
      }
    }
  }
}
