#include "RoundedRaffTheme.h"

#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "fontIds.h"

namespace {
constexpr int kCoverRadius = 18;
constexpr int kMenuRadius = 30;
constexpr int kBottomRadius = 15;
constexpr int kRowRadius = 20;
constexpr int kInteractiveInsetX = 20;
constexpr int kSelectableRowGap = 6;
constexpr int batteryPercentSpacing = 4;
constexpr int kTitleFontId = UI_12_FONT_ID;     // Requested main title size: 12px
constexpr int kSubtitleFontId = SMALL_FONT_ID;  // Requested subtitle size: 8px
constexpr int kGuideFontId = SMALL_FONT_ID;     // Closest available to requested 6px

void drawScrollBar(const GfxRenderer& renderer, Rect rect, int itemCount, int pageStartIndex, int pageItems) {
  if (itemCount <= 0 || pageItems <= 0 || itemCount <= pageItems) {
    return;
  }

  const int barW = RoundedRaffMetrics::values.scrollBarWidth;
  const int barX = rect.x + rect.width - RoundedRaffMetrics::values.scrollBarRightOffset - barW;
  const int barY = rect.y;
  const int barH = rect.height;

  const int thumbH = std::max(10, (barH * pageItems) / itemCount);
  const int maxStart = std::max(1, itemCount - pageItems);
  const int maxTravel = std::max(1, barH - thumbH);
  const int thumbY = barY + (pageStartIndex * maxTravel) / maxStart;

  renderer.fillRect(barX, thumbY, barW, thumbH);
}

void drawBatteryIcon(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight, uint16_t percentage) {
  // Top line
  renderer.drawLine(x + 1, y, x + battWidth - 3, y);
  // Bottom line
  renderer.drawLine(x + 1, y + rectHeight - 1, x + battWidth - 3, y + rectHeight - 1);
  // Left line
  renderer.drawLine(x, y + 1, x, y + rectHeight - 2);
  // Battery end
  renderer.drawLine(x + battWidth - 2, y + 1, x + battWidth - 2, y + rectHeight - 2);
  renderer.drawPixel(x + battWidth - 1, y + 3);
  renderer.drawPixel(x + battWidth - 1, y + rectHeight - 4);
  renderer.drawLine(x + battWidth - 0, y + 4, x + battWidth - 0, y + rectHeight - 5);

  // The +1 is to round up, so that we always fill at least one pixel.
  int filledWidth = percentage * (battWidth - 5) / 100 + 1;
  if (filledWidth > battWidth - 5) {
    filledWidth = battWidth - 5;  // Ensure we don't overflow.
  }

  renderer.fillRect(x + 2, y + 2, filledWidth, rectHeight - 4);
}

void drawBatteryRightStable(const GfxRenderer& renderer, Rect iconRect, uint16_t percentage, bool showPercentage) {
  // Match BaseTheme::drawBatteryRight layout, but use a stable percentage value for this render.
  const int iconY = iconRect.y + 6;

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, percentageText.c_str());
    renderer.drawText(SMALL_FONT_ID, iconRect.x - textWidth - batteryPercentSpacing, iconRect.y,
                      percentageText.c_str());
  }

  drawBatteryIcon(renderer, iconRect.x, iconY, RoundedRaffMetrics::values.batteryWidth, iconRect.height, percentage);
}

std::string sanitizeButtonLabel(std::string label) {
  // Remove common directional prefixes/symbols (e.g. "<< Home", unsupported icon glyphs).
  while (!label.empty() && !std::isalnum(static_cast<unsigned char>(label[0]))) {
    label.erase(0, 1);
  }
  // Trim any extra left spaces.
  while (!label.empty() && label[0] == ' ') {
    label.erase(0, 1);
  }
  return label;
}

}  // namespace
int coverWidth = 0;

void RoundedRaffTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                                  const char* subtitle) const {
  (void)subtitle;
  // Home screen header is custom-rendered in drawRecentBookCover.
  if (title == nullptr) {
    return;
  }
  const int sidePadding = RoundedRaffMetrics::values.contentSidePadding;
  const int titleX = rect.x + sidePadding;
  const int titleY = rect.y + 14;

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const uint16_t percentage = powerManager.getBatteryPercentage();
  const int batteryIconX = rect.x + rect.width - sidePadding - RoundedRaffMetrics::values.batteryWidth;
  int batteryGroupLeftX = batteryIconX;
  if (showBatteryPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    batteryGroupLeftX -= renderer.getTextWidth(SMALL_FONT_ID, percentageText.c_str()) + batteryPercentSpacing;

    // Clear a fixed-width area for the battery percentage to avoid ghosting when digit count changes (e.g. 100% ->
    // 99%).
    const int maxTextWidth = renderer.getTextWidth(SMALL_FONT_ID, "100%");
    const int clearW = maxTextWidth + batteryPercentSpacing + RoundedRaffMetrics::values.batteryWidth;
    const int clearH = std::max(renderer.getTextHeight(SMALL_FONT_ID), RoundedRaffMetrics::values.batteryHeight + 8);
    renderer.fillRect(batteryIconX - maxTextWidth - batteryPercentSpacing, rect.y + 14, clearW, clearH, false);
  }

  const int maxTextWidth = std::max(0, batteryGroupLeftX - 20 - titleX);
  auto headerTitle = renderer.truncatedText(kTitleFontId, title, maxTextWidth, EpdFontFamily::BOLD);
  renderer.drawText(kTitleFontId, titleX, titleY, headerTitle.c_str(), true, EpdFontFamily::BOLD);
  drawBatteryRightStable(renderer,
                         Rect{batteryIconX, rect.y + 14, RoundedRaffMetrics::values.batteryWidth,
                              RoundedRaffMetrics::values.batteryHeight},
                         percentage, showBatteryPercentage);
}

void RoundedRaffTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                                  bool selected) const {
  if (tabs.empty()) {
    return;
  }

  const int slotWidth = rect.width / static_cast<int>(tabs.size());
  const int tabY = rect.y + 4;
  const int tabHeight = rect.height - 12;

  for (size_t i = 0; i < tabs.size(); i++) {
    const int slotX = rect.x + static_cast<int>(i) * slotWidth;
    const int tabX = slotX + 4;
    const int tabWidth = slotWidth - 8;
    const auto& tab = tabs[i];

    if (tab.selected) {
      renderer.fillRoundedRect(tabX, tabY, tabWidth, tabHeight, 18, selected ? Color::Black : Color::DarkGray);
    }

    const int textWidth = renderer.getTextWidth(kTitleFontId, tab.label, EpdFontFamily::BOLD);
    const int textX = slotX + (slotWidth - textWidth) / 2;
    const int textY = tabY + (tabHeight - renderer.getLineHeight(kTitleFontId)) / 2;
    renderer.drawText(kTitleFontId, textX, textY, tab.label, !(tab.selected), EpdFontFamily::BOLD);
  }

  // Full-width divider between tabs and setting rows.
  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width, rect.y + rect.height - 1, true);
}

void RoundedRaffTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                           bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const int tileWidth = rect.width - 2 * RoundedRaffMetrics::values.contentSidePadding;
  const int tileHeight = rect.height;
  const int tileY = rect.y;
  const bool hasContinueReading = !recentBooks.empty();
  if (coverWidth == 0) {
    coverWidth = RoundedRaffMetrics::values.homeCoverHeight * 0.6;
  }
  const int imgY = tileY + (tileHeight - RoundedRaffMetrics::values.homeCoverHeight) / 2;
  const int tileX = RoundedRaffMetrics::values.contentSidePadding;

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    RecentBook book = recentBooks[0];
    if (!coverRendered) {
      std::string coverPath = book.coverBmpPath;
      bool hasCover = true;
      int tileX = RoundedRaffMetrics::values.contentSidePadding;
      if (coverPath.empty()) {
        hasCover = false;
      } else {
        const std::string coverBmpPath =
            UITheme::getCoverThumbPath(coverPath, RoundedRaffMetrics::values.homeCoverHeight);

        // First time: load cover from SD and render
        FsFile file;
        if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
          Bitmap bitmap(file);
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            coverWidth = bitmap.getWidth();
            renderer.drawBitmap(bitmap, tileX + (tileWidth - coverWidth) / 2, imgY, coverWidth,
                                RoundedRaffMetrics::values.homeCoverHeight);
            renderer.maskRoundedRectOutsideCorners(tileX + (tileWidth - coverWidth) / 2, imgY, coverWidth,
                                                   RoundedRaffMetrics::values.homeCoverHeight, kCoverRadius,
                                                   Color::LightGray);
          } else {
            hasCover = false;
          }
          file.close();
        }
      }

      // Draw either way
      renderer.drawRoundedRect(tileX + (tileWidth - coverWidth) / 2, imgY, coverWidth,
                               RoundedRaffMetrics::values.homeCoverHeight, 1, kCoverRadius, true);

      if (!hasCover) {
        // Render empty cover
        renderer.fillRect(tileX + (tileWidth - coverWidth) / 2, imgY + (RoundedRaffMetrics::values.homeCoverHeight / 3),
                          coverWidth, 2 * RoundedRaffMetrics::values.homeCoverHeight / 3, true);
        renderer.drawIcon(CoverIcon, tileX + (tileWidth - coverWidth) / 2 + 24, imgY + 24, 32, 32);
        renderer.maskRoundedRectOutsideCorners(tileX + (tileWidth - coverWidth) / 2, imgY, coverWidth,
                                               RoundedRaffMetrics::values.homeCoverHeight, kCoverRadius,
                                               Color::LightGray);
      }

      coverBufferStored = storeCoverBuffer();
      coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer
    }

    renderer.fillRoundedRect(tileX, tileY, tileWidth, imgY - tileY, kRowRadius, true, true, false, false,
                             Color::LightGray);
    renderer.fillRectDither(tileX, imgY, (tileWidth - coverWidth) / 2, RoundedRaffMetrics::values.homeCoverHeight,
                            Color::LightGray);
    renderer.fillRectDither(tileX + (tileWidth + coverWidth) / 2, imgY, (tileWidth - coverWidth) / 2,
                            RoundedRaffMetrics::values.homeCoverHeight, Color::LightGray);
    renderer.fillRoundedRect(tileX, imgY + RoundedRaffMetrics::values.homeCoverHeight, tileWidth,
                             tileHeight - (imgY - tileY + RoundedRaffMetrics::values.homeCoverHeight), kRowRadius,
                             false, false, true, true, Color::LightGray);
  } else {
    renderer.fillRoundedRect(tileX, tileY, tileWidth, tileHeight, kRowRadius, Color::LightGray);
    renderer.drawCenteredText(kTitleFontId, rect.y + rect.height / 2 - renderer.getLineHeight(kTitleFontId) / 2,
                              tr(STR_NO_OPEN_BOOK));
  }
}

void RoundedRaffTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                      const std::function<std::string(int index)>& buttonLabel,
                                      const std::function<UIIcon(int index)>& rowIcon) const {
  (void)rowIcon;
  const int sidePadding = RoundedRaffMetrics::values.contentSidePadding;
  const int rowX = rect.x + sidePadding;
  const int rowHeight = renderer.getLineHeight(kTitleFontId) + 20;  // 10px top + 10px bottom
  const int rowGap = kSelectableRowGap;
  const int rowStep = rowHeight + rowGap;
  const int pageItems = std::max(1, rect.height / rowStep);
  const int safeSelectedIndex = std::max(0, selectedIndex);
  const int pageStartIndex = (safeSelectedIndex / pageItems) * pageItems;
  const int menuTop = rect.y;
  const int textLineHeight = renderer.getLineHeight(kTitleFontId);
  const int menuMaxWidth = std::max(0, rect.width - sidePadding * 2);

  for (int i = pageStartIndex; i < buttonCount && i < pageStartIndex + pageItems; ++i) {
    const std::string label = buttonLabel(i);
    const int rowY = menuTop + (i - pageStartIndex) * rowStep;
    constexpr int kRowPaddingX = 40;  // 20px L/R
    const int maxLabelWidth = std::max(0, menuMaxWidth - kRowPaddingX);
    const std::string truncatedLabel =
        renderer.truncatedText(kTitleFontId, label.c_str(), maxLabelWidth, EpdFontFamily::BOLD);
    const int rowWidth = std::min(
        menuMaxWidth, renderer.getTextWidth(kTitleFontId, truncatedLabel.c_str(), EpdFontFamily::BOLD) + kRowPaddingX);
    const bool isSelected = selectedIndex == i;
    renderer.fillRoundedRect(rowX, rowY, rowWidth, rowHeight, kMenuRadius, isSelected ? Color::Black : Color::White);
    const int textY = rowY + (rowHeight - textLineHeight) / 2;
    const int textX = rowX + kInteractiveInsetX;
    if (selectedIndex == i) {
      renderer.drawText(kTitleFontId, textX, textY, truncatedLabel.c_str(), false, EpdFontFamily::BOLD);
    } else {
      renderer.drawText(kTitleFontId, textX, textY, truncatedLabel.c_str(), true, EpdFontFamily::BOLD);
    }
  }

  drawScrollBar(renderer, rect, buttonCount, pageStartIndex, pageItems);
}

void RoundedRaffTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                                const std::function<std::string(int index)>& rowTitle,
                                const std::function<std::string(int index)>& rowSubtitle,
                                const std::function<UIIcon(int index)>& rowIcon,
                                const std::function<std::string(int index)>& rowValue, bool highlightValue) const {
  (void)rowIcon;
  (void)highlightValue;
  const bool hasSubtitle = static_cast<bool>(rowSubtitle);
  const int titleLineHeight = renderer.getLineHeight(kTitleFontId);
  const int subtitleLineHeight = renderer.getLineHeight(kSubtitleFontId);
  constexpr int subtitleTopPadding = 10;
  constexpr int subtitleBottomPadding = 10;
  constexpr int subtitleInterLineGap = 4;
  const int subtitleRowHeight =
      subtitleTopPadding + titleLineHeight + subtitleInterLineGap + subtitleLineHeight + subtitleBottomPadding;
  const int rowHeight = hasSubtitle ? subtitleRowHeight : RoundedRaffMetrics::values.listRowHeight;
  const int rowStep = rowHeight + kSelectableRowGap;
  const int pageItems = std::max(1, rect.height / rowStep);
  const int pageStartIndex = std::max(0, selectedIndex / pageItems) * pageItems;

  const int sidePadding = RoundedRaffMetrics::values.contentSidePadding;
  const int rowX = rect.x + sidePadding;
  const int rowWidth = rect.width - sidePadding * 2;

  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int rowY = rect.y + (i % pageItems) * rowStep;
    const bool isSelected = i == selectedIndex;
    renderer.fillRoundedRect(rowX, rowY, rowWidth, rowHeight, kRowRadius, isSelected ? Color::Black : Color::White);

    constexpr int kMinTitleWidth = 40;
    constexpr int kMinValueGap = kInteractiveInsetX;
    int textAreaWidth = rowWidth - kInteractiveInsetX * 2;
    if (rowValue) {
      std::string valueText = rowValue(i);
      if (!valueText.empty()) {
        const int maxValueWidth = std::max(0, rowWidth - kInteractiveInsetX * 2 - kMinValueGap - kMinTitleWidth);
        if (maxValueWidth > 0) {
          const std::string truncatedValue =
              renderer.truncatedText(kTitleFontId, valueText.c_str(), maxValueWidth, EpdFontFamily::REGULAR);
          const int valueW = renderer.getTextWidth(kTitleFontId, truncatedValue.c_str(), EpdFontFamily::REGULAR);
          renderer.drawText(kTitleFontId, rowX + rowWidth - kInteractiveInsetX - valueW,
                            rowY + (rowHeight - renderer.getLineHeight(kTitleFontId)) / 2, truncatedValue.c_str(),
                            !isSelected, EpdFontFamily::REGULAR);
          textAreaWidth = std::max(0, textAreaWidth - valueW - kMinValueGap);
        }
      }
    }

    if (hasSubtitle) {
      const std::string subtitleRaw = rowSubtitle(i);
      auto title = renderer.truncatedText(kTitleFontId, rowTitle(i).c_str(), textAreaWidth, EpdFontFamily::BOLD);

      if (subtitleRaw.empty()) {
        // If there is no subtitle/author, center title vertically in the full row.
        const int centeredTitleY = rowY + (rowHeight - titleLineHeight) / 2;
        renderer.drawText(kTitleFontId, rowX + kInteractiveInsetX, centeredTitleY, title.c_str(), !isSelected,
                          EpdFontFamily::BOLD);
      } else {
        const int titleY = rowY + subtitleTopPadding;
        const int subtitleY = titleY + titleLineHeight + subtitleInterLineGap;
        auto subtitle =
            renderer.truncatedText(kSubtitleFontId, subtitleRaw.c_str(), textAreaWidth, EpdFontFamily::REGULAR);
        renderer.drawText(kTitleFontId, rowX + kInteractiveInsetX, titleY, title.c_str(), !isSelected,
                          EpdFontFamily::BOLD);
        renderer.drawText(kSubtitleFontId, rowX + kInteractiveInsetX, subtitleY, subtitle.c_str(), !isSelected,
                          EpdFontFamily::REGULAR);
      }
    } else {
      auto title = renderer.truncatedText(kTitleFontId, rowTitle(i).c_str(), textAreaWidth, EpdFontFamily::BOLD);
      renderer.drawText(kTitleFontId, rowX + kInteractiveInsetX,
                        rowY + (rowHeight - renderer.getLineHeight(kTitleFontId)) / 2, title.c_str(), !isSelected,
                        EpdFontFamily::BOLD);
    }
  }

  drawScrollBar(renderer, rect, itemCount, pageStartIndex, pageItems);
}

void RoundedRaffTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                       const char* btn4) const {
  const GfxRenderer::Orientation origOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int sidePadding = 20;
  const int groupGap = 10;
  const int bottomMargin = 10;
  const int hintHeight = RoundedRaffMetrics::values.buttonHintsHeight - 10;  // 30px total guide height
  const int groupWidth = (pageWidth - sidePadding * 2 - groupGap) / 2;
  const int hintY = pageHeight - hintHeight - bottomMargin;
  const int textY = hintY + (hintHeight - renderer.getLineHeight(kGuideFontId)) / 2;

  const bool backDisabled = (btn1 == nullptr || btn1[0] == '\0');
  const int leftGroupX = sidePadding;
  const int rightGroupX = leftGroupX + groupWidth + groupGap;
  const std::string backLabel = backDisabled ? "" : sanitizeButtonLabel(std::string(btn1));
  // Callers should provide the button labels. If a label is not specified, it should render empty.
  const std::string selectText = (btn2 && btn2[0] != '\0') ? sanitizeButtonLabel(std::string(btn2)) : "";
  const std::string upText = (btn3 && btn3[0] != '\0') ? sanitizeButtonLabel(std::string(btn3)) : "";
  const std::string downText = (btn4 && btn4[0] != '\0') ? sanitizeButtonLabel(std::string(btn4)) : "";

  // Ensure button hints always "win" visually even if other elements accidentally render into this area.
  renderer.fillRect(leftGroupX, hintY, groupWidth, hintHeight, false);
  renderer.fillRect(rightGroupX, hintY, groupWidth, hintHeight, false);

  renderer.drawRoundedRect(leftGroupX, hintY, groupWidth, hintHeight, 2, kBottomRadius, true);
  const int selectWidth = renderer.getTextWidth(kGuideFontId, selectText.c_str(), EpdFontFamily::REGULAR);
  const int downWidth = renderer.getTextWidth(kGuideFontId, downText.c_str(), EpdFontFamily::REGULAR);
  constexpr int innerEdgePadding = 16;

  const int backX = leftGroupX + innerEdgePadding;
  const int selectX = leftGroupX + groupWidth - innerEdgePadding - selectWidth;
  const int upX = rightGroupX + innerEdgePadding;
  const int downX = rightGroupX + groupWidth - innerEdgePadding - downWidth;

  if (!backDisabled) {
    renderer.drawText(kGuideFontId, backX, textY, backLabel.c_str(), true, EpdFontFamily::REGULAR);
  }
  renderer.drawText(kGuideFontId, selectX, textY, selectText.c_str(), true, EpdFontFamily::REGULAR);

  renderer.drawRoundedRect(rightGroupX, hintY, groupWidth, hintHeight, 2, kBottomRadius, true);

  renderer.drawText(kGuideFontId, upX, textY, upText.c_str(), true, EpdFontFamily::REGULAR);
  renderer.drawText(kGuideFontId, downX, textY, downText.c_str(), true, EpdFontFamily::REGULAR);

  renderer.setOrientation(origOrientation);
}
