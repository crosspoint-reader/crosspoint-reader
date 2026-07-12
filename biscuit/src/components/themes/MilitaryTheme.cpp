#include "MilitaryTheme.h"

#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int cornerBracketLen = 12;
constexpr int cornerBracketThick = 2;
constexpr int frameBorderOuter = 2;
constexpr int frameBorderGap = 3;
constexpr int frameBorderInner = 1;
constexpr int maxListValueWidth = 200;

// Draw L-shaped corner brackets at the four corners of a rect
void drawCornerBrackets(const GfxRenderer& renderer, int x, int y, int w, int h) {
  // Top-left
  renderer.fillRect(x, y, cornerBracketLen, cornerBracketThick);
  renderer.fillRect(x, y, cornerBracketThick, cornerBracketLen);
  // Top-right
  renderer.fillRect(x + w - cornerBracketLen, y, cornerBracketLen, cornerBracketThick);
  renderer.fillRect(x + w - cornerBracketThick, y, cornerBracketThick, cornerBracketLen);
  // Bottom-left
  renderer.fillRect(x, y + h - cornerBracketThick, cornerBracketLen, cornerBracketThick);
  renderer.fillRect(x, y + h - cornerBracketLen, cornerBracketThick, cornerBracketLen);
  // Bottom-right
  renderer.fillRect(x + w - cornerBracketLen, y + h - cornerBracketThick, cornerBracketLen, cornerBracketThick);
  renderer.fillRect(x + w - cornerBracketThick, y + h - cornerBracketLen, cornerBracketThick, cornerBracketLen);
}

// Draw double border frame: outer 2px + gap + inner 1px
void drawDoubleFrame(const GfxRenderer& renderer, int x, int y, int w, int h) {
  // Outer border
  renderer.drawRect(x, y, w, h, frameBorderOuter, true);
  // Inner border
  const int inset = frameBorderOuter + frameBorderGap;
  renderer.drawRect(x + inset, y + inset, w - inset * 2, h - inset * 2, frameBorderInner, true);
}

// Draw a horizontal dashed line
void drawDashedLine(const GfxRenderer& renderer, int x1, int x2, int y, int dashLen = 4, int gapLen = 3) {
  bool drawing = true;
  int count = 0;
  for (int x = x1; x <= x2; x++) {
    if (drawing) {
      renderer.drawPixel(x, y);
    }
    count++;
    if (drawing && count >= dashLen) {
      drawing = false;
      count = 0;
    } else if (!drawing && count >= gapLen) {
      drawing = true;
      count = 0;
    }
  }
}

}  // namespace

// --- Header: inverted full-width bar with UPPERCASE monospace title ---
void MilitaryTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  // Fill header bar black (inverted)
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height);

  // Battery in header (white on black)
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const int batteryX = rect.x + rect.width - 12 - MilitaryMetrics::values.batteryWidth;
  drawBatteryRight(renderer,
                   Rect{batteryX, rect.y + 5, MilitaryMetrics::values.batteryWidth, MilitaryMetrics::values.batteryHeight},
                   showBatteryPercentage);

  if (title) {
    // Convert title to uppercase
    std::string upperTitle(title);
    for (auto& c : upperTitle) {
      c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    }

    int maxTitleWidth = rect.width - MilitaryMetrics::values.contentSidePadding * 2;
    auto truncatedTitle = renderer.truncatedText(UI_10_FONT_ID, upperTitle.c_str(), maxTitleWidth, EpdFontFamily::BOLD);
    // White text on black bar (inverted = false)
    renderer.drawText(UI_10_FONT_ID, rect.x + MilitaryMetrics::values.contentSidePadding, rect.y + 8,
                      truncatedTitle.c_str(), false, EpdFontFamily::BOLD);
  }

  if (subtitle) {
    std::string upperSub(subtitle);
    for (auto& c : upperSub) {
      c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    }
    auto truncatedSubtitle =
        renderer.truncatedText(SMALL_FONT_ID, upperSub.c_str(),
                               rect.width - MilitaryMetrics::values.contentSidePadding * 2, EpdFontFamily::REGULAR);
    int subWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedSubtitle.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - MilitaryMetrics::values.contentSidePadding - subWidth,
                      rect.y + 10, truncatedSubtitle.c_str(), false);
  }
}

// --- SubHeader: dashed underline separator ---
void MilitaryTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                                  const char* rightLabel) const {
  int currentX = rect.x + MilitaryMetrics::values.contentSidePadding;
  int rightSpace = MilitaryMetrics::values.contentSidePadding;

  if (rightLabel) {
    auto truncatedRightLabel =
        renderer.truncatedText(SMALL_FONT_ID, rightLabel, maxListValueWidth, EpdFontFamily::REGULAR);
    int rightLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedRightLabel.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - MilitaryMetrics::values.contentSidePadding - rightLabelWidth,
                      rect.y + 7, truncatedRightLabel.c_str());
    rightSpace += rightLabelWidth + 10;
  }

  // Label in uppercase
  std::string upperLabel(label);
  for (auto& c : upperLabel) {
    c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
  }
  auto truncatedLabel = renderer.truncatedText(
      UI_10_FONT_ID, upperLabel.c_str(), rect.width - MilitaryMetrics::values.contentSidePadding - rightSpace,
      EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, currentX, rect.y, truncatedLabel.c_str(), true, EpdFontFamily::BOLD);

  // Dashed line under the sub-header
  const int lineY = rect.y + renderer.getLineHeight(UI_10_FONT_ID) + 4;
  drawDashedLine(renderer, rect.x + MilitaryMetrics::values.contentSidePadding,
                 rect.x + rect.width - MilitaryMetrics::values.contentSidePadding, lineY);
}

// --- Tab bar: inverted selected tab, sharp corners ---
void MilitaryTheme::drawTabBar(const GfxRenderer& renderer, const Rect rect, const std::vector<TabInfo>& tabs,
                               bool selected) const {
  int currentX = rect.x + MilitaryMetrics::values.contentSidePadding;

  for (const auto& tab : tabs) {
    // Uppercase tab labels
    std::string upperLabel(tab.label);
    for (auto& c : upperLabel) {
      c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    }

    const int textWidth =
        renderer.getTextWidth(UI_10_FONT_ID, upperLabel.c_str(), tab.selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

    if (tab.selected) {
      if (selected) {
        // Active + focused: fully inverted
        renderer.fillRect(currentX - 4, rect.y, textWidth + 8, lineHeight + 4);
        renderer.drawText(UI_10_FONT_ID, currentX, rect.y, upperLabel.c_str(), false, EpdFontFamily::BOLD);
      } else {
        // Active but not focused: underline
        renderer.fillRect(currentX, rect.y + lineHeight + 2, textWidth, 2);
        renderer.drawText(UI_10_FONT_ID, currentX, rect.y, upperLabel.c_str(), true, EpdFontFamily::BOLD);
      }
    } else {
      renderer.drawText(UI_10_FONT_ID, currentX, rect.y, upperLabel.c_str(), true, EpdFontFamily::REGULAR);
    }

    currentX += textWidth + MilitaryMetrics::values.tabSpacing;
  }
}

// --- List: inverted selection with > prefix, dashed separators ---
void MilitaryTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                             const std::function<std::string(int index)>& rowTitle,
                             const std::function<std::string(int index)>& rowSubtitle,
                             const std::function<UIIcon(int index)>& rowIcon,
                             const std::function<std::string(int index)>& rowValue, bool highlightValue) const {
  const int rowHeight =
      (rowSubtitle != nullptr) ? MilitaryMetrics::values.listWithSubtitleRowHeight : MilitaryMetrics::values.listRowHeight;
  const int pageItems = rect.height / rowHeight;

  // Scroll indicators
  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    constexpr int indicatorWidth = 20;
    constexpr int arrowSize = 6;
    constexpr int margin = 15;
    const int centerX = rect.x + rect.width - indicatorWidth / 2 - margin;
    const int indicatorTop = rect.y;
    const int indicatorBottom = rect.y + rect.height - arrowSize;

    // Up arrow
    for (int i = 0; i < arrowSize; ++i) {
      const int lineWidth = 1 + i * 2;
      renderer.drawLine(centerX - i, indicatorTop + i, centerX - i + lineWidth - 1, indicatorTop + i);
    }
    // Down arrow
    for (int i = 0; i < arrowSize; ++i) {
      const int lineWidth = 1 + (arrowSize - 1 - i) * 2;
      const int startX = centerX - (arrowSize - 1 - i);
      renderer.drawLine(startX, indicatorBottom - arrowSize + 1 + i, startX + lineWidth - 1,
                        indicatorBottom - arrowSize + 1 + i);
    }
  }

  const int contentWidth = rect.width - 5;
  const int prefixWidth = renderer.getTextWidth(UI_10_FONT_ID, "> ");
  const int pageStartIndex = selectedIndex / pageItems * pageItems;

  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int itemY = rect.y + (i % pageItems) * rowHeight;
    const bool isSelected = (i == selectedIndex);

    // Selection: inverted full-width bar
    if (isSelected) {
      renderer.fillRect(0, itemY - 2, rect.width, rowHeight);
    }

    // Draw > prefix for selected item
    int textX = rect.x + MilitaryMetrics::values.contentSidePadding;
    int textWidth = contentWidth - MilitaryMetrics::values.contentSidePadding * 2 - (rowValue != nullptr ? 60 : 0);

    if (isSelected) {
      renderer.drawText(UI_10_FONT_ID, textX, itemY, "> ", false, EpdFontFamily::BOLD);
      textX += prefixWidth;
      textWidth -= prefixWidth;
    }

    // Title
    auto itemName = rowTitle(i);
    // Uppercase for titles
    for (auto& c : itemName) {
      c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    }
    auto font = (rowSubtitle != nullptr) ? UI_12_FONT_ID : UI_10_FONT_ID;
    auto item = renderer.truncatedText(font, itemName.c_str(), textWidth);
    renderer.drawText(font, textX, itemY, item.c_str(), !isSelected);

    // Subtitle
    if (rowSubtitle != nullptr) {
      std::string subtitleText = rowSubtitle(i);
      auto subtitle = renderer.truncatedText(UI_10_FONT_ID, subtitleText.c_str(), textWidth);
      renderer.drawText(UI_10_FONT_ID, isSelected ? textX : rect.x + MilitaryMetrics::values.contentSidePadding,
                        itemY + 28, subtitle.c_str(), !isSelected);
    }

    // Value
    if (rowValue != nullptr) {
      std::string valueText = rowValue(i);
      const auto valueTextWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());
      renderer.drawText(UI_10_FONT_ID,
                        rect.x + contentWidth - MilitaryMetrics::values.contentSidePadding - valueTextWidth, itemY,
                        valueText.c_str(), !isSelected);
    }

    // Dashed separator between items (not after last visible item)
    if (!isSelected && i < pageStartIndex + pageItems - 1 && i < itemCount - 1) {
      drawDashedLine(renderer, rect.x + MilitaryMetrics::values.contentSidePadding,
                     rect.x + rect.width - MilitaryMetrics::values.contentSidePadding, itemY + rowHeight - 3);
    }
  }
}

// --- Button hints: bracket-style labels ---
void MilitaryTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                    const char* btn4) const {
  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = 106;
  constexpr int buttonHeight = MilitaryMetrics::values.buttonHintsHeight;
  constexpr int buttonY = MilitaryMetrics::values.buttonHintsHeight;
  constexpr int textYOffset = 7;
  constexpr int buttonPositions[] = {25, 130, 245, 350};
  const char* labels[] = {btn1, btn2, btn3, btn4};

  for (int i = 0; i < 4; i++) {
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int x = buttonPositions[i];
      const int y = pageHeight - buttonY;

      // Clear area
      renderer.fillRect(x, y, buttonWidth, buttonHeight, false);
      // Sharp rect border (no rounding)
      renderer.drawRect(x, y, buttonWidth, buttonHeight);

      if (!BaseTheme::drawArrowIfNeeded(renderer, labels[i], x + buttonWidth / 2, y + buttonHeight / 2, 6, true)) {
        // Format as [LABEL]
        std::string bracketLabel = std::string("[") + labels[i] + "]";
        // Uppercase
        for (size_t j = 1; j < bracketLabel.size() - 1; j++) {
          bracketLabel[j] = static_cast<char>(toupper(static_cast<unsigned char>(bracketLabel[j])));
        }

        const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, bracketLabel.c_str());
        const int textX = x + (buttonWidth - 1 - textWidth) / 2;
        renderer.drawText(UI_10_FONT_ID, textX, y + textYOffset, bracketLabel.c_str());
      }
    }
  }

  renderer.setOrientation(orig_orientation);
}

// --- Button menu: sharp corners, inverted selection with > prefix ---
void MilitaryTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                   const std::function<std::string(int index)>& buttonLabel,
                                   const std::function<UIIcon(int index)>& rowIcon) const {
  for (int i = 0; i < buttonCount; ++i) {
    const int tileY = MilitaryMetrics::values.verticalSpacing + rect.y +
                      static_cast<int>(i) * (MilitaryMetrics::values.menuRowHeight + MilitaryMetrics::values.menuSpacing);

    const bool selected = selectedIndex == i;

    if (selected) {
      renderer.fillRect(rect.x + MilitaryMetrics::values.contentSidePadding, tileY,
                        rect.width - MilitaryMetrics::values.contentSidePadding * 2,
                        MilitaryMetrics::values.menuRowHeight);
    } else {
      renderer.drawRect(rect.x + MilitaryMetrics::values.contentSidePadding, tileY,
                        rect.width - MilitaryMetrics::values.contentSidePadding * 2,
                        MilitaryMetrics::values.menuRowHeight);
    }

    std::string labelStr = buttonLabel(i);
    // Uppercase
    for (auto& c : labelStr) {
      c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    }

    // Add > prefix for selected
    if (selected) {
      labelStr = "> " + labelStr;
    }

    const char* label = labelStr.c_str();
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
    const int textX = rect.x + (rect.width - textWidth) / 2;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int textY = tileY + (MilitaryMetrics::values.menuRowHeight - lineHeight) / 2;
    renderer.drawText(UI_10_FONT_ID, textX, textY, label, !selected);
  }
}

// --- Progress bar: text-based |||||||---- style ---
void MilitaryTheme::drawProgressBar(const GfxRenderer& renderer, Rect rect, const size_t current,
                                    const size_t total) const {
  if (total == 0) {
    return;
  }

  const int percent = static_cast<int>((static_cast<uint64_t>(current) * 100) / total);

  // Draw sharp outline
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);

  // Filled portion
  const int fillWidth = (rect.width - 4) * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(rect.x + 2, rect.y + 2, fillWidth, rect.height - 4);
  }

  // Text-based percentage below
  std::string progressText;
  constexpr int barChars = 20;
  const int filledChars = barChars * percent / 100;
  for (int i = 0; i < barChars; i++) {
    progressText += (i < filledChars) ? '|' : '-';
  }
  progressText += " " + std::to_string(percent) + "%";
  renderer.drawCenteredText(UI_10_FONT_ID, rect.y + rect.height + 15, progressText.c_str());
}

// --- Popup: double border with corner brackets ---
Rect MilitaryTheme::drawPopup(const GfxRenderer& renderer, const char* message) const {
  constexpr int margin = 20;
  constexpr int y = 60;

  // Uppercase message
  std::string upperMsg(message);
  for (auto& c : upperMsg) {
    c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
  }

  const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, upperMsg.c_str(), EpdFontFamily::BOLD);
  const int textHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int w = textWidth + margin * 2;
  const int h = textHeight + margin * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;

  // White fill
  renderer.fillRect(x - 6, y - 6, w + 12, h + 12, false);

  // Double frame
  drawDoubleFrame(renderer, x - 4, y - 4, w + 8, h + 8);

  // Corner brackets
  drawCornerBrackets(renderer, x - 4, y - 4, w + 8, h + 8);

  // Text
  const int textX = x + (w - textWidth) / 2;
  const int textY = y + margin - 2;
  renderer.drawText(UI_10_FONT_ID, textX, textY, upperMsg.c_str(), true, EpdFontFamily::BOLD);
  renderer.displayBuffer();
  return Rect{x, y, w, h};
}

// --- Popup progress: filled rect with percentage ---
void MilitaryTheme::fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const {
  constexpr int barHeight = 4;
  const int barWidth = layout.width - 30;
  const int barX = layout.x + (layout.width - barWidth) / 2;
  const int barY = layout.y + layout.height - 10;

  const int fillWidth = barWidth * progress / 100;
  renderer.fillRect(barX, barY, fillWidth, barHeight, true);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

// --- Text field: sharp brackets ---
void MilitaryTheme::drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth) const {
  renderer.drawText(UI_10_FONT_ID, rect.x + 8, rect.y, "[");
  renderer.drawText(UI_10_FONT_ID, rect.x + rect.width - 14, rect.y + rect.height, "]");
  // Draw underline for the input area
  renderer.drawLine(rect.x + 20, rect.y + rect.height, rect.x + rect.width - 20, rect.y + rect.height);
}

// --- Keyboard key: inverted when selected ---
void MilitaryTheme::drawKeyboardKey(const GfxRenderer& renderer, Rect rect, const char* label,
                                    const bool isSelected) const {
  const int itemWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
  const int textX = rect.x + (rect.width - itemWidth) / 2;

  if (isSelected) {
    // Inverted key
    renderer.fillRect(rect.x, rect.y - 2, rect.width, rect.height);
    renderer.drawText(UI_10_FONT_ID, textX, rect.y, label, false);
  } else {
    renderer.drawText(UI_10_FONT_ID, textX, rect.y, label);
  }
}

// --- Help text: uppercase ---
void MilitaryTheme::drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) const {
  std::string upperLabel(label);
  for (auto& c : upperLabel) {
    c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
  }
  const auto& metrics = UITheme::getInstance().getMetrics();
  auto truncatedLabel = renderer.truncatedText(SMALL_FONT_ID, upperLabel.c_str(),
                                               rect.width - metrics.contentSidePadding * 2, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(SMALL_FONT_ID, rect.y, truncatedLabel.c_str());
}
