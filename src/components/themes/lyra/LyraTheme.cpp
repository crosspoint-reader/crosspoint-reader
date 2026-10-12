#include "LyraTheme.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "UiFontScale.h"
#include "components/BookProgressPresentation.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/book24.h"
#include "components/icons/cover.h"
#include "components/icons/ellipsis.h"
#include "components/icons/file24.h"
#include "components/icons/folder.h"
#include "components/icons/folder24.h"
#include "components/icons/image24.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/stats.h"
#include "components/icons/text24.h"
#include "components/icons/transfer.h"
#include "components/icons/weather24.h"
#include "components/icons/weather32.h"
#include "components/themes/ButtonHintLayout.h"
#include "components/themes/CardTextFit.h"
#include "components/themes/ListTouchBand.h"
#include "components/themes/TapTargets.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;
constexpr int topHintButtonY = 345;
constexpr int popupMarginX = 16;
constexpr int popupMarginY = 12;
constexpr int maxListValueWidth = 200;
constexpr int mainMenuIconSize = 32;
constexpr int listIconSize = 24;
constexpr int mainMenuColumns = 2;
constexpr int minAdaptiveMenuRowHeight = 40;

int coverWidth = 0;

void drawLyraBatteryIcon(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight,
                         uint16_t percentage) {
  BaseTheme::drawBatteryOutline(renderer, x, y, battWidth, rectHeight);

  const bool charging = gpio.isUsbConnected();

  if (charging) {
    // Draw solid fill when charging so lightning bolt is visible
    renderer.fillRect(x + 2, y + 2, battWidth - 5, rectHeight - 4);
    BaseTheme::drawBatteryLightningBolt(renderer, x + 4, y + 2);
  } else {
    // Draw bars when not charging
    if (percentage > 10) {
      renderer.fillRect(x + 2, y + 2, 3, rectHeight - 4);
    }
    if (percentage > 40) {
      renderer.fillRect(x + 6, y + 2, 3, rectHeight - 4);
    }
    if (percentage > 70) {
      renderer.fillRect(x + 10, y + 2, 3, rectHeight - 4);
    }
  }
}

}  // namespace

const uint8_t* LyraTheme::iconForName(UIIcon icon, int size) {
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
      case UIIcon::Weather:
        return Weather24Icon;
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
      case UIIcon::Stats:
        return StatsIcon;
      case UIIcon::Ellipsis:
        return EllipsisIcon;
      case UIIcon::Settings:
        return Settings2Icon;
      case UIIcon::Transfer:
        return TransferIcon;
      case UIIcon::Library:
        return LibraryIcon;
      case UIIcon::Weather:
        return Weather32Icon;
      default:
        return nullptr;
    }
  }
  return nullptr;
}

// Reads the overall progress percent for a recent book. Delegates to
// BookProgressPresentation so the progress.bin layout lives in one place.
// Returns -1 if the file is absent or the percent byte is not yet written.
int LyraTheme::getRecentBookProgressPercent(const RecentBook& book) {
  return BookProgressPresentation::readPercent(book);
}

void LyraTheme::drawBatteryLeft(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  // Left aligned: icon on left, percentage on right (reader mode)
  const uint16_t percentage = powerManager.getBatteryPercentage();

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    renderer.drawText(SMALL_FONT_ID, rect.x + BaseTheme::batteryPercentSpacing + LyraMetrics::values.batteryWidth,
                      rect.y, percentageText.c_str());
  }

  drawLyraBatteryIcon(renderer, rect.x, rect.y + 6, LyraMetrics::values.batteryWidth, rect.height, percentage);
}

void LyraTheme::drawBatteryRight(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  // Right aligned: percentage on left, icon on right (UI headers)
  const uint16_t percentage = powerManager.getBatteryPercentage();

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, percentageText.c_str());
    // Clear the area where we're going to draw the text to prevent ghosting
    const auto textHeight = renderer.getTextHeight(SMALL_FONT_ID);
    renderer.fillRect(rect.x - textWidth - BaseTheme::batteryPercentSpacing, rect.y, textWidth, textHeight, false);
    // Draw text to the left of the icon
    renderer.drawText(SMALL_FONT_ID, rect.x - textWidth - BaseTheme::batteryPercentSpacing, rect.y,
                      percentageText.c_str());
  }

  drawLyraBatteryIcon(renderer, rect.x, rect.y + 6, LyraMetrics::values.batteryWidth, rect.height, percentage);
}

void LyraTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  // Position icon at right edge, drawBatteryRight will place text to the left
  const int batteryX = rect.x + rect.width - 12 - LyraMetrics::values.batteryWidth;
  drawBatteryRight(renderer,
                   Rect{batteryX, rect.y + 5, LyraMetrics::values.batteryWidth, LyraMetrics::values.batteryHeight},
                   showBatteryPercentage);

  // Draw clock in header
  if (SETTINGS.useClock) {
    char clockStr[16];
    HalClock::formatTime(clockStr, sizeof(clockStr), !SETTINGS.clockFormat12h);
    renderer.drawText(SMALL_FONT_ID, rect.x + LyraMetrics::values.contentSidePadding, rect.y + 5, clockStr);
  }

  int maxTitleWidth = title != nullptr ? renderer.getTextWidth(UI_12_FONT_ID, title, EpdFontFamily::BOLD) : 0;
  int maxSubtitleWidth =
      subtitle != nullptr ? renderer.getTextWidth(SMALL_FONT_ID, subtitle, EpdFontFamily::REGULAR) : 0;

  // Available space is the distance between the side paddings, and a with side padding between title and subtitle.
  const int availableSpace = rect.width - LyraMetrics::values.contentSidePadding * 3;

  if (maxTitleWidth + maxSubtitleWidth > availableSpace) {
    if ((maxTitleWidth > availableSpace / 2) && (maxSubtitleWidth > availableSpace / 2)) {
      // Both are wider then half the space, truncate both.
      maxTitleWidth = availableSpace / 2;
      maxSubtitleWidth = availableSpace / 2;
    } else {
      // Truncate the the longest one
      if (maxTitleWidth > maxSubtitleWidth) {
        maxTitleWidth = availableSpace - maxSubtitleWidth;
      } else {
        maxSubtitleWidth = availableSpace - maxTitleWidth;
      }
    }
  }

  if (title) {
    auto truncatedTitle = renderer.truncatedText(UI_12_FONT_ID, title, maxTitleWidth, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, rect.x + LyraMetrics::values.contentSidePadding,
                      rect.y + LyraMetrics::values.batteryBarHeight + 3, truncatedTitle.c_str(), true,
                      EpdFontFamily::BOLD);
    renderer.drawLine(rect.x, rect.y + rect.height - 3, rect.x + rect.width - 1, rect.y + rect.height - 3, 3, true);
  }

  if (subtitle) {
    auto truncatedSubtitle = renderer.truncatedText(SMALL_FONT_ID, subtitle, maxSubtitleWidth, EpdFontFamily::REGULAR);
    int truncatedSubtitleWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedSubtitle.c_str());
    renderer.drawText(SMALL_FONT_ID,
                      rect.x + rect.width - LyraMetrics::values.contentSidePadding - truncatedSubtitleWidth,
                      rect.y + 50, truncatedSubtitle.c_str(), true);
  }
}

void LyraTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  const int contentWidth = std::max(0, rect.width - LyraMetrics::values.contentSidePadding * 2);

  // The right label is truncated against the space actually available, not a fixed 200px cap.
  // The cap predates any caller that puts something long there: the Calibre screen's right label
  // is the device IP, and a three-digit-octet address clipped to a stub the user cannot type in.
  // Ported from crosspoint-reader PR #3235 (FDKevin / @fdkevin0).
  int labelWidth = contentWidth;
  if (rightLabel) {
    auto truncatedRightLabel = renderer.truncatedText(SMALL_FONT_ID, rightLabel, contentWidth, EpdFontFamily::REGULAR);
    const int rightLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedRightLabel.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - LyraMetrics::values.contentSidePadding - rightLabelWidth,
                      rect.y + 7, truncatedRightLabel.c_str());
    labelWidth = std::max(0, contentWidth - rightLabelWidth - hPaddingInSelection);
  }

  if (labelWidth > 0) {
    auto truncatedLabel = renderer.truncatedText(UI_10_FONT_ID, label, labelWidth, EpdFontFamily::REGULAR);
    renderer.drawText(UI_10_FONT_ID, rect.x + LyraMetrics::values.contentSidePadding, rect.y + 6,
                      truncatedLabel.c_str(), true, EpdFontFamily::REGULAR);
  }

  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

void LyraTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<UIIcon(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue, bool highlightValue) const {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const UiFontLadder::Step growth = UITheme::fontGrowth();
  int rowHeight = (rowSubtitle != nullptr) ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight;
  int pageItems = rect.height / rowHeight;
  // Never paint more rows than the touch band can register: a painted row past the cap is drawn
  // but recorded by nothing, so it answers to no tap. That is not hypothetical -- the reader
  // menu on the T5S3's 540x960 portrait frame fits 28 rows against a cap of 24, and its bottom
  // four rows were dead. Clamping here costs blank space at the foot of an over-tall list
  // instead, and keeps "every painted row is a tappable row" true on any future panel.
  pageItems = std::min(pageItems, ListTouchBand::kMaxRows);
  if (pageItems <= 0 || itemCount <= 0 || rowTitle == nullptr) {
    ListTouchBand::invalidate();
    return;
  }

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    const int scrollAreaHeight = rect.height;

    // Draw scroll bar
    const int scrollBarHeight = (scrollAreaHeight * pageItems) / itemCount;
    const int currentPage = selectedIndex / pageItems;
    const int scrollBarY = rect.y + ((scrollAreaHeight - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - LyraMetrics::values.scrollBarRightOffset;
    renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + scrollAreaHeight, true);
    renderer.fillRect(scrollBarX - LyraMetrics::values.scrollBarWidth, scrollBarY, LyraMetrics::values.scrollBarWidth,
                      scrollBarHeight, true);
  }

  bool selectedIsSeparator = false;
  if (selectedIndex >= 0 && selectedIndex < itemCount && rowTitle != nullptr) {
    selectedIsSeparator = UITheme::isSeparatorTitle(rowTitle(selectedIndex));
  }

  // Draw selection
  int contentWidth =
      rect.width -
      (totalPages > 1 ? (LyraMetrics::values.scrollBarWidth + LyraMetrics::values.scrollBarRightOffset) : 1);
  if (selectedIndex >= 0 && !selectedIsSeparator) {
    renderer.fillRoundedRect(
        rect.x + LyraMetrics::values.contentSidePadding, rect.y + selectedIndex % pageItems * rowHeight,
        contentWidth - LyraMetrics::values.contentSidePadding * 2, rowHeight, cornerRadius, Color::LightGray);
  }

  int textX = rect.x + LyraMetrics::values.contentSidePadding + hPaddingInSelection;
  int textWidth = contentWidth - LyraMetrics::values.contentSidePadding * 2 - hPaddingInSelection * 2;
  int iconSize = 0;
  int iconY = 0;
  if (rowIcon != nullptr) {
    iconSize = (rowSubtitle != nullptr) ? mainMenuIconSize : listIconSize;
    iconY = (rowHeight - iconSize) / 2;
    textX += iconSize + hPaddingInSelection;
    textWidth -= iconSize + hPaddingInSelection;
  }

  // Draw all items
  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
  // Rows published for touch as they are painted — see BaseTheme::drawList for why this is
  // recorded from the draw rather than re-derived by each screen. Full rect.width, matching
  // where the row's own selection fill reads as a target.
  ListTouchBand::Builder touchBand;
  touchBand.begin(rect.x, rect.width, pageStartIndex);

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
    const bool isSeparator = UITheme::isSeparatorTitle(itemName);
    touchBand.addRow(itemY, rowHeight, !isSeparator);
    if (isSeparator) {
      itemName = UITheme::stripSeparatorTitle(itemName);
      drawListSeparator(renderer,
                        Rect{rect.x + LyraMetrics::values.contentSidePadding, itemY,
                             contentWidth - LyraMetrics::values.contentSidePadding * 2, rowHeight},
                        textX, rowTextWidth, itemName);
      continue;
    }

    auto item = renderer.truncatedText(UI_10_FONT_ID, itemName.c_str(), rowTextWidth);
    renderer.drawText(UI_10_FONT_ID, textX, itemY + 7, item.c_str(), true);

    if (rowIcon != nullptr) {
      UIIcon icon = rowIcon(i);
      const uint8_t* iconBitmap = iconForName(icon, iconSize);
      int renderSize = iconSize;
      if (iconBitmap == nullptr && rowSubtitle != nullptr) {
        renderSize = listIconSize;
        iconBitmap = iconForName(icon, renderSize);
      }
      if (iconBitmap != nullptr) {
        renderer.drawIcon(iconBitmap, rect.x + LyraMetrics::values.contentSidePadding + hPaddingInSelection,
                          itemY + iconY, renderSize, renderSize);
      }
    }

    if (rowSubtitle != nullptr) {
      std::string subtitleText = rowSubtitle(i);
      const auto nl = subtitleText.find('\n');
      // The three offsets are tuned to overlap the line boxes slightly, so they are kept as-is
      // and each line is pushed down by the growth of the lines ABOVE it at the current UI font
      // size. Re-deriving them from line heights instead would undo that tuning.
      if (nl != std::string::npos) {
        // Two-line subtitle: first line (author) at +24, second line (series) at +40
        auto line1 = renderer.truncatedText(SMALL_FONT_ID, subtitleText.substr(0, nl).c_str(), rowTextWidth);
        renderer.drawText(SMALL_FONT_ID, textX, itemY + 24 + growth.body, line1.c_str(), true);
        auto line2 = renderer.truncatedText(SMALL_FONT_ID, subtitleText.substr(nl + 1).c_str(), rowTextWidth);
        renderer.drawText(SMALL_FONT_ID, textX, itemY + 40 + growth.body + growth.small, line2.c_str(), true);
      } else {
        auto subtitle = renderer.truncatedText(SMALL_FONT_ID, subtitleText.c_str(), rowTextWidth);
        renderer.drawText(SMALL_FONT_ID, textX, itemY + 30 + growth.body, subtitle.c_str(), true);
      }
    }

    // Draw value
    if (!valueText.empty()) {
      if (i == selectedIndex && highlightValue) {
        renderer.fillRoundedRect(
            rect.x + contentWidth - LyraMetrics::values.contentSidePadding - hPaddingInSelection - valueWidth, itemY,
            valueWidth + hPaddingInSelection, rowHeight, cornerRadius, Color::Black);
      }

      renderer.drawText(UI_10_FONT_ID, rect.x + contentWidth - LyraMetrics::values.contentSidePadding - valueWidth,
                        itemY + 6, valueText.c_str(), !(i == selectedIndex && highlightValue));
    }
  }

  touchBand.commit();
}

void LyraTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) const {
  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  // Inverted draws the strip inverted too, so the labels are the right way up for the reader —
  // see BaseTheme::drawButtonHints for why that works and why landscape is a separate problem.
  const bool inverted = orig_orientation == GfxRenderer::Orientation::PortraitInverted;
  renderer.setDrawOrientation(inverted ? GfxRenderer::Orientation::PortraitInverted
                                       : GfxRenderer::Orientation::Portrait);

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = 80;
  constexpr int smallButtonHeight = 15;
  // Read live rather than from the constexpr table: the strip grows with the UI font size.
  const int buttonHeight = UITheme::getInstance().getMetrics().buttonHintsHeight;
  const int buttonY = buttonHeight;  // Distance from bottom
  constexpr int textYOffset = 7;     // Distance from top of button to text baseline
  // Hand-tuned for the widths named in ButtonHintLayout; other panels spread evenly.
  constexpr int x4ButtonPositions[] = {58, 146, 254, 342};
  constexpr int x3ButtonPositions[] = {65, 157, 291, 383};
  int buttonPositions[4];
  ButtonHintLayout::positions(renderer.getScreenWidth(), buttonWidth, x4ButtonPositions, x3ButtonPositions,
                              buttonPositions);
  const char* labels[] = {btn1, btn2, btn3, btn4};
  // A box grows when its label needs it, and the strip makes room around it.
  int boxX[4];
  int boxWidth[4];
  layOutButtonHints(renderer, SMALL_FONT_ID, buttonWidth, buttonPositions, labels, boxX, boxWidth);

  // Inverted flips both axes: the strip's panel-bottom band becomes the top one and each slot
  // mirrors across the width, which is what carries every box back to its own button.
  const int fullY = inverted ? 0 : pageHeight - buttonY;
  const int smallY = inverted ? 0 : pageHeight - smallButtonHeight;
  // Only three sides are stroked: the edge that sits on the panel border is left open. Mirroring
  // the box mirrors which edge that is, so the corner flags (which also gate the straight edges)
  // have to flip too — otherwise inverted strokes the screen edge and leaves the content-facing
  // side of the box unpainted.
  const bool roundTop = !inverted;
  const bool roundBottom = inverted;

  // Publish the geometry for touch, in PORTRAIT coordinates — the frame ButtonHintStrip
  // documents and the hit test resolves into. The un-mirrored values are correct for
  // inverted too, and not by accident: drawing a box at (pageWidth - pos - width, 0) in the
  // PortraitInverted frame puts it on the same panel spot as (pos, pageHeight - buttonY) in
  // the Portrait frame, so the two mirrors cancel. Recording the mirrored values instead
  // would hit-test inverted taps against the opposite button.
  //
  // Only the full-size boxes are tappable: an empty label still paints here, but as a
  // decorative stub of smallButtonHeight with no meaning behind it, so a finger landing on
  // one must do nothing.
  ButtonHintStrip::Strip strip;
  strip.y = pageHeight - buttonY;
  strip.height = buttonHeight;
  for (int i = 0; i < 4; i++) {
    strip.x[i] = boxX[i];
    strip.width[i] = boxWidth[i];
    strip.active[i] = labels[i] != nullptr && labels[i][0] != '\0';
  }
  ButtonHintStrip::record(strip);

  for (int i = 0; i < 4; i++) {
    const int width = boxWidth[i];
    const int x = inverted ? pageWidth - boxX[i] - width : boxX[i];
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      // Draw the filled background and border for a FULL-sized button
      renderer.fillRoundedRect(x, fullY, width, buttonHeight, cornerRadius, Color::White);
      // Fit the label to the box. The box grows with its label only up to a fair quarter of the
      // strip, while the label's font grows with the UI font size, so a long label can still run
      // out of BOTH ends of its own box -- the text is centred, so the overflow is symmetrical --
      // and sprawl across its neighbours. fitHintLabel() shrinks it, then clips it.
      const FittedHintLabel label =
          fitHintLabel(renderer, SMALL_FONT_ID, labels[i], width - HintLayout::BOTTOM_LABEL_PADDING);
      const int textWidth = renderer.getTextWidth(label.fontId, label.text.c_str());
      const int textX = x + (width - 1 - textWidth) / 2;
      renderer.drawText(label.fontId, textX, fullY + textYOffset, label.text.c_str());
      renderer.drawRoundedRect(x, fullY, width, buttonHeight, 1, cornerRadius, roundTop, roundTop, roundBottom,
                               roundBottom, true);
    } else {
      // Draw the filled background and border for a SMALL-sized button
      renderer.fillRoundedRect(x, smallY, width, smallButtonHeight, cornerRadius, Color::White);
      renderer.drawRoundedRect(x, smallY, width, smallButtonHeight, 1, cornerRadius, roundTop, roundTop, roundBottom,
                               roundBottom, true);
    }
  }

  renderer.setDrawOrientation(orig_orientation);
}

void LyraTheme::drawSideButtonHints(GfxRenderer& renderer, const char* upBtn, const char* downBtn) const {
  // Panel coordinates and the inverted mirroring, for the reasons spelled out in
  // BaseTheme::drawSideButtonHints — including why LandscapeCounterClockwise draws inverted too.
  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  // Taken before the draw is forced into the panel frame -- see BaseTheme::drawSideButtonHints.
  const HintLayout::Span span = sideHintSpan(renderer);
  const int needed = sideHintNeededLength(renderer, SMALL_FONT_ID, upBtn, downBtn);
  const bool inverted = orig_orientation == GfxRenderer::Orientation::PortraitInverted ||
                        orig_orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  renderer.setDrawOrientation(inverted ? GfxRenderer::Orientation::PortraitInverted
                                       : GfxRenderer::Orientation::Portrait);

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = LyraMetrics::values.sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 78;  // Tuned height on screen (width when rotated); a longer label grows it
  constexpr int buttonMargin = 0;
  const bool hasUp = upBtn != nullptr && upBtn[0] != '\0';
  const bool hasDown = downBtn != nullptr && downBtn[0] != '\0';

  const auto roundedRect = [&](const int x, const int y, const int w, const int h, const bool topLeft,
                               const bool topRight, const bool bottomLeft, const bool bottomRight) {
    // Mirroring the box mirrors which of its corners are the rounded ones too.
    renderer.drawRoundedRect(inverted ? screenWidth - x - w : x, inverted ? screenHeight - y - h : y, w, h, 1,
                             cornerRadius, inverted ? bottomRight : topLeft, inverted ? bottomLeft : topRight,
                             inverted ? topRight : bottomLeft, inverted ? topLeft : bottomRight, true);
  };
  // Fits the label to its box (y, length) and centres it there.
  const auto label = [&](const int x, const int y, const int length, const char* text) {
    const FittedHintLabel fitted = fitHintLabel(renderer, SMALL_FONT_ID, text, length - HintLayout::SIDE_LABEL_PADDING);
    const int textWidth = renderer.getTextWidth(fitted.fontId, fitted.text.c_str());
    const int textHeight = renderer.getTextHeight(fitted.fontId);
    const int textY = y + (length + textWidth) / 2;
    renderer.drawTextRotated90CW(fitted.fontId, inverted ? screenWidth - x - textHeight - 5 : x,
                                 inverted ? screenHeight - 1 - textY + textWidth : textY, fitted.text.c_str());
  };

  if (ButtonGeometry::sideKeysOnOppositeEdges()) {
    // X3 / X4 Pro layout: Up on left side, Down on right side, positioned higher
    constexpr int x3ButtonY = 155;
    const int rightX = screenWidth - buttonWidth;
    const HintLayout::Box box = HintLayout::sideCentred(x3ButtonY + buttonHeight / 2, buttonHeight, needed, span);

    // Publish geometry for touch, in the same un-mirrored logical coordinates roundedRect()
    // mirrors from -- see LyraTheme::drawButtonHints for why the strip is recorded un-inverted
    // even when the draw itself flips for PortraitInverted.
    ButtonHintStrip::SideStrip strip;
    strip.x[0] = buttonMargin;
    strip.y[0] = box.y;
    strip.width[0] = buttonWidth;
    strip.height[0] = box.height;
    strip.active[0] = hasUp;
    strip.x[1] = rightX;
    strip.y[1] = box.y;
    strip.width[1] = buttonWidth;
    strip.height[1] = box.height;
    strip.active[1] = hasDown;
    ButtonHintStrip::Side::record(strip);

    if (hasUp) {
      roundedRect(buttonMargin, box.y, buttonWidth, box.height, false, true, false, true);
      label(buttonMargin, box.y, box.height, upBtn);
    }

    if (hasDown) {
      roundedRect(rightX, box.y, buttonWidth, box.height, true, false, true, false);
      label(rightX, box.y, box.height, downBtn);
    }
  } else {
    // X4 layout: Both buttons stacked on right side
    const int x = screenWidth - buttonWidth;
    const HintLayout::Stack stack =
        HintLayout::sideStacked(topHintButtonY + buttonHeight, /*gap=*/5, buttonHeight, needed, span);
    const HintLayout::Box& up = stack.up;
    const HintLayout::Box& down = stack.down;

    ButtonHintStrip::SideStrip strip;
    strip.x[0] = x;
    strip.y[0] = up.y;
    strip.width[0] = buttonWidth;
    strip.height[0] = up.height;
    strip.active[0] = hasUp;
    strip.x[1] = x;
    strip.y[1] = down.y;
    strip.width[1] = buttonWidth;
    strip.height[1] = down.height;
    strip.active[1] = hasDown;
    ButtonHintStrip::Side::record(strip);

    if (hasUp) {
      roundedRect(x, up.y, buttonWidth, up.height, true, false, true, false);
      label(x, up.y, up.height, upBtn);
    }

    if (hasDown) {
      roundedRect(x, down.y, buttonWidth, down.height, true, false, true, false);
      label(x, down.y, down.height, downBtn);
    }
  }

  renderer.setDrawOrientation(orig_orientation);
}

void LyraTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const int tileWidth = rect.width - 2 * LyraMetrics::values.contentSidePadding;
  const int tileHeight = rect.height;
  const int tileY = rect.y;
  const int coverHeight = std::max(120, tileHeight - 2 * hPaddingInSelection);
  const bool hasContinueReading = !recentBooks.empty();
  if (coverWidth == 0) {
    coverWidth = static_cast<int>(coverHeight * 0.6f);
  }

  // Tile published for touch, ahead of the draw so a cached repaint (coverRendered) records it
  // too — see BaseTheme::drawRecentBookCover. One cover, so value 0.
  {
    TapTargets::Recorder::Builder coverTargets;
    if (hasContinueReading) {
      coverTargets.add(rect.x + LyraMetrics::values.contentSidePadding, tileY, tileWidth, tileHeight, 0);
    }
    TapTargets::homeCovers().record(coverTargets);
  }

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    RecentBook book = recentBooks[0];
    const int progressPercent = getRecentBookProgressPercent(book);
    if (!coverRendered) {
      std::string coverPath = book.coverBmpPath;
      bool hasCover = true;
      bool coverPending = false;
      int tileX = LyraMetrics::values.contentSidePadding;
      if (coverPath.empty()) {
        hasCover = false;
      } else {
        const std::string coverBmpPath = UITheme::getCoverThumbPath(coverPath, coverHeight);

        // First time: load cover from SD and render
        FsFile file;
        if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
          Bitmap bitmap(file);
          // Never draw the no-cover marker: scaling a 1x1 BMP into the slot paints a solid block.
          // Falling through to the existing no-cover branch shows the theme's own tile instead.
          if (bitmap.parseHeaders() == BmpReaderError::Ok &&
              !UITheme::isCoverPlaceholderBmp(bitmap.getWidth(), bitmap.getHeight())) {
            coverWidth = bitmap.getWidth();
            renderer.fillRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection, coverWidth, coverHeight, false);
            renderer.drawBitmap(bitmap, tileX + hPaddingInSelection, tileY + hPaddingInSelection, coverWidth,
                                coverHeight);
          } else {
            hasCover = false;
          }
          file.close();
        } else {
          hasCover = false;
          coverPending = true;  // path exists but BMP not ready yet
        }
      }

      // Draw either way
      renderer.drawRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection, coverWidth, coverHeight, true);

      if (!hasCover) {
        if (coverPending) {
          // Cover is being generated — show a loading label centred in the cover area
          const char* loadingText = tr(STR_LOADING);
          const int textW = renderer.getTextWidth(SMALL_FONT_ID, loadingText);
          const int textH = renderer.getLineHeight(SMALL_FONT_ID);
          renderer.drawText(SMALL_FONT_ID, tileX + hPaddingInSelection + (coverWidth - textW) / 2,
                            tileY + hPaddingInSelection + (coverHeight - textH) / 2, loadingText, true);
        } else {
          // No cover at all — render empty cover placeholder
          renderer.fillRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection + (coverHeight / 3), coverWidth,
                            2 * coverHeight / 3, true);
          renderer.drawIcon(CoverIcon, tileX + hPaddingInSelection + 24, tileY + hPaddingInSelection + 24, 32, 32);
        }
      }

      // Only cache the frame buffer once the cover is definitively resolved (loaded or confirmed absent).
      // If a cover is still being generated we keep coverRendered=false so the next render will retry.
      if (!coverPending) {
        coverBufferStored = storeCoverBuffer();
        coverRendered = coverBufferStored;
      }
    }

    bool bookSelected = (selectorIndex == 0);

    int tileX = LyraMetrics::values.contentSidePadding;
    int textWidth = tileWidth - 2 * hPaddingInSelection - LyraMetrics::values.verticalSpacing - coverWidth;

    if (bookSelected) {
      // Draw selection box
      renderer.fillRoundedRect(tileX, tileY, tileWidth, hPaddingInSelection, cornerRadius, true, true, false, false,
                               Color::LightGray);
      renderer.fillRectDither(tileX, tileY + hPaddingInSelection, hPaddingInSelection, coverHeight, Color::LightGray);
      renderer.fillRectDither(tileX + hPaddingInSelection + coverWidth, tileY + hPaddingInSelection,
                              tileWidth - hPaddingInSelection - coverWidth, coverHeight, Color::LightGray);
      renderer.fillRoundedRect(tileX, tileY + coverHeight + hPaddingInSelection, tileWidth, hPaddingInSelection,
                               cornerRadius, false, false, true, true, Color::LightGray);
    }

    // Progress + pace-based ETA, e.g. "62% · ~45m". Replaces the old top-right
    // percent badge on this layout — the percentage now lives in the text line.
    const std::string statusLine = BookProgressPresentation::formatStatus(book, progressPercent);

    auto titleLines = renderer.wrappedText(UI_12_FONT_ID, book.title.c_str(), textWidth, 3, EpdFontFamily::BOLD);
    auto authorLines = renderer.wrappedText(UI_10_FONT_ID, book.author.c_str(), textWidth, 2);
    auto seriesLines = renderer.wrappedText(UI_10_FONT_ID, book.series.c_str(), textWidth, 2);

    const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int smallLineHeight = renderer.getLineHeight(UI_10_FONT_ID);

    // What you have put into the book, under what is left of it. This column is only the part of
    // the tile the cover does not use, so the sentence usually needs two lines; it is set in the
    // non-scaling small face so a larger UI font cannot make the row itself any taller.
    const std::string history = BookProgressPresentation::historyLine(book);
    const int historyLineHeight = renderer.getLineHeight(FIT_SMALL_FONT_ID);
    auto historyLines = history.empty() ? std::vector<std::string>{}
                                        : renderer.wrappedText(FIT_SMALL_FONT_ID, history.c_str(), textWidth, 2);

    // Each part wrapped to its own cap can sum to more than the tile: a long title, a list of
    // co-authors and a long series name did (#375). CardTextFit decides what gives way, gaps first.
    const int gap = smallLineHeight / 2;
    constexpr int tightGap = 4;
    CardTextFit::Parts parts{};
    parts[CardTextFit::Title] = {titleLineHeight, static_cast<int>(titleLines.size()), 0};
    parts[CardTextFit::Author] = {smallLineHeight, static_cast<int>(authorLines.size()), gap};
    // Author and series are one group under one gap.
    parts[CardTextFit::Series] = {smallLineHeight, static_cast<int>(seriesLines.size()), authorLines.empty() ? gap : 0};
    parts[CardTextFit::Status] = {smallLineHeight, statusLine.empty() ? 0 : 1, gap};
    parts[CardTextFit::History] = {historyLineHeight, static_cast<int>(historyLines.size()), gap};
    const CardTextFit::Result fit = CardTextFit::fit(parts, tileHeight - 2 * hPaddingInSelection, tightGap);

    const auto rewrap = [&](std::vector<std::string>& lines, const CardTextFit::PartId part, const int fontId,
                            const std::string& text, const EpdFontFamily::Style style) {
      if (fit.lines[part] < static_cast<int>(lines.size())) {
        lines = renderer.wrappedText(fontId, text.c_str(), textWidth, fit.lines[part], style);
      }
    };
    rewrap(titleLines, CardTextFit::Title, UI_12_FONT_ID, book.title, EpdFontFamily::BOLD);
    rewrap(authorLines, CardTextFit::Author, UI_10_FONT_ID, book.author, EpdFontFamily::REGULAR);
    rewrap(seriesLines, CardTextFit::Series, UI_10_FONT_ID, book.series, EpdFontFamily::REGULAR);
    if (fit.lines[CardTextFit::History] < static_cast<int>(historyLines.size())) {
      historyLines.clear();
      if (fit.lines[CardTextFit::History] > 0) {
        const std::string compact = BookProgressPresentation::historyLineCompact(book);
        historyLines.push_back(renderer.truncatedText(FIT_SMALL_FONT_ID, compact.c_str(), textWidth));
      }
    }

    // Centred in the tile. A block too tall even at its leanest is held to the top of the
    // selection, so what overflows is the bottom rather than the title.
    const int blockY = std::max(tileY + hPaddingInSelection, tileY + tileHeight / 2 - fit.height / 2);
    const int textX = tileX + hPaddingInSelection + coverWidth + LyraMetrics::values.verticalSpacing;
    const auto drawPart = [&](const std::vector<std::string>& lines, const CardTextFit::PartId part, const int fontId,
                              const EpdFontFamily::Style style) {
      int y = blockY + fit.tops[part];
      for (const auto& line : lines) {
        renderer.drawText(fontId, textX, y, line.c_str(), true, style);
        y += parts[part].lineHeight;
      }
    };
    drawPart(titleLines, CardTextFit::Title, UI_12_FONT_ID, EpdFontFamily::BOLD);
    drawPart(authorLines, CardTextFit::Author, UI_10_FONT_ID, EpdFontFamily::REGULAR);
    drawPart(seriesLines, CardTextFit::Series, UI_10_FONT_ID, EpdFontFamily::REGULAR);
    if (!statusLine.empty()) {
      renderer.drawText(UI_10_FONT_ID, textX, blockY + fit.tops[CardTextFit::Status], statusLine.c_str(), true,
                        EpdFontFamily::BOLD);
    }
    drawPart(historyLines, CardTextFit::History, FIT_SMALL_FONT_ID, EpdFontFamily::REGULAR);
  } else {
    drawEmptyRecents(renderer, rect);
  }
}

void LyraTheme::drawEmptyRecents(const GfxRenderer& renderer, const Rect rect) const {
  constexpr int padding = 48;
  renderer.drawText(UI_12_FONT_ID, rect.x + padding,
                    rect.y + rect.height / 2 - renderer.getLineHeight(UI_12_FONT_ID) - 2, tr(STR_NO_OPEN_BOOK), true,
                    EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, rect.x + padding, rect.y + rect.height / 2 + 2, tr(STR_START_READING), true);
}

void LyraTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  int rowHeight = UITheme::getInstance().getMetrics().menuRowHeight;
  int rowSpacing = LyraMetrics::values.menuSpacing;
  if (buttonCount > 0 && rect.height > 0) {
    const int defaultHeight = buttonCount * rowHeight + std::max(0, buttonCount - 1) * rowSpacing;
    if (defaultHeight > rect.height) {
      const int spacingSlots = std::max(1, buttonCount - 1);
      rowSpacing = std::max(0, (rect.height - buttonCount * rowHeight) / spacingSlots);
      if (buttonCount * rowHeight + std::max(0, buttonCount - 1) * rowSpacing > rect.height) {
        rowHeight =
            std::max(minAdaptiveMenuRowHeight, (rect.height - std::max(0, buttonCount - 1) * rowSpacing) / buttonCount);
      }
      if (buttonCount * rowHeight + std::max(0, buttonCount - 1) * rowSpacing > rect.height) {
        rowHeight = std::max(1, rect.height / buttonCount);
        rowSpacing = 0;
      }
    }
  }

  // Rows published for touch — see BaseTheme::drawButtonMenu. Values are MENU-LOCAL indices.
  TapTargets::Recorder::Builder menuTargets;

  for (int i = 0; i < buttonCount; ++i) {
    int tileWidth = rect.width - LyraMetrics::values.contentSidePadding * 2;
    Rect tileRect = Rect{rect.x + LyraMetrics::values.contentSidePadding, rect.y + i * (rowHeight + rowSpacing),
                         tileWidth, rowHeight};
    menuTargets.add(tileRect.x, tileRect.y, tileRect.width, tileRect.height, i);

    const bool selected = selectedIndex == i;

    if (selected) {
      renderer.fillRoundedRect(tileRect.x, tileRect.y, tileRect.width, tileRect.height, cornerRadius, Color::LightGray);
    }

    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    int textX = tileRect.x + 16;
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int textY = tileRect.y + (rowHeight - lineHeight) / 2;

    if (rowIcon != nullptr) {
      UIIcon icon = rowIcon(i);
      int iconSize = mainMenuIconSize;
      const uint8_t* iconBitmap = iconForName(icon, iconSize);
      if (iconBitmap == nullptr) {
        // Some icons only have 24px variants; fall back so they still render.
        iconSize = listIconSize;
        iconBitmap = iconForName(icon, iconSize);
      }
      if (iconBitmap != nullptr) {
        renderer.drawIcon(iconBitmap, textX, textY + 3, iconSize, iconSize);
        textX += iconSize + hPaddingInSelection + 2;
      }
    }

    renderer.drawText(UI_12_FONT_ID, textX, textY, label, true);
  }

  TapTargets::homeMenu().record(menuTargets);
}

Rect LyraTheme::drawPopup(const GfxRenderer& renderer, const char* message, const bool overlayDisplayedFrame,
                          const PopupShip ship) const {
  // See BaseTheme::drawPopup: overlay the box on the displayed frame unless the caller composed its
  // own full frame into the write buffer.
  if (overlayDisplayedFrame) renderer.syncWriteBufferFromDisplayed();
  constexpr int y = 132;
  constexpr int outline = 2;
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, message, EpdFontFamily::REGULAR);
  const int textHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int w = textWidth + popupMarginX * 2;
  const int h = textHeight + popupMarginY * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;

  renderer.fillRoundedRect(x - outline, y - outline, w + outline * 2, h + outline * 2, cornerRadius + outline,
                           Color::White);
  renderer.fillRoundedRect(x, y, w, h, cornerRadius, Color::Black);

  const int textX = x + (w - textWidth) / 2;
  const int textY = y + popupMarginY - 2;
  renderer.drawText(UI_12_FONT_ID, textX, textY, message, false, EpdFontFamily::REGULAR);
  shipPopup(renderer, ship);

  return Rect{x, y, w, h};
}

void LyraTheme::fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const {
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

void LyraTheme::drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode,
                              int contentStartX, int contentWidth) const {
  int lineY = rect.y + rect.height + renderer.getLineHeight(UI_12_FONT_ID) + LyraMetrics::values.verticalSpacing;
  const int thickness = cursorMode ? 3 : 2;
  if (contentWidth > 0) {
    renderer.drawLine(rect.x + contentStartX, lineY, rect.x + contentStartX + contentWidth, lineY, thickness, true);
  } else {
    int lineW = textWidth + hPaddingInSelection * 2;
    renderer.drawLine(rect.x + (rect.width - lineW) / 2, lineY, rect.x + (rect.width + lineW) / 2, lineY, thickness,
                      true);
  }
}
