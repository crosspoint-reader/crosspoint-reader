#include "LyraTheme.h"

#include <GfxRenderer.h>
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
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/book24.h"
#include "components/icons/bookmark.h"
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
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;
constexpr int topHintButtonY = 345;
constexpr int maxListValueWidth = 200;
constexpr int mainMenuIconSize = 32;
constexpr int listIconSize = 24;
constexpr int mainMenuColumns = 2;
int coverWidth = 0;

bool isBmpIconSize(int size) { return size > 0 && size <= 64; }

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
      case UIIcon::Bookmark:
        return BookmarkIcon;
      default:
        return nullptr;
    }
  }
  return nullptr;
}

enum class ButtonHintShape { None, Back, Select, Up, Down, Left, Right };

bool matchesLabel(const char* label, const char* expected) {
  return label != nullptr && expected != nullptr && strcmp(label, expected) == 0;
}

ButtonHintShape shapeForButtonHintLabel(const char* label) {
  if (label == nullptr || label[0] == '\0') return ButtonHintShape::None;
  if (matchesLabel(label, tr(STR_BACK)) || matchesLabel(label, tr(STR_CANCEL)) || matchesLabel(label, tr(STR_HOME))) {
    return ButtonHintShape::Back;
  }
  if (matchesLabel(label, tr(STR_SELECT)) || matchesLabel(label, tr(STR_CONFIRM)) ||
      matchesLabel(label, tr(STR_OK_BUTTON)) || matchesLabel(label, tr(STR_DONE)) ||
      matchesLabel(label, tr(STR_OPEN)) || matchesLabel(label, tr(STR_TOGGLE))) {
    return ButtonHintShape::Select;
  }
  if (matchesLabel(label, tr(STR_DIR_UP))) return ButtonHintShape::Up;
  if (matchesLabel(label, tr(STR_DIR_DOWN))) return ButtonHintShape::Down;
  if (matchesLabel(label, tr(STR_DIR_LEFT)) || strcmp(label, "<") == 0 || strcmp(label, "-") == 0) {
    return ButtonHintShape::Left;
  }
  if (matchesLabel(label, tr(STR_DIR_RIGHT)) || strcmp(label, ">") == 0 || strcmp(label, "+") == 0) {
    return ButtonHintShape::Right;
  }
  return ButtonHintShape::None;
}

void fillCircle(const GfxRenderer& renderer, int cx, int cy, int radius) {
  const int r2 = radius * radius;
  for (int y = -radius; y <= radius; ++y) {
    for (int x = -radius; x <= radius; ++x) {
      if (x * x + y * y <= r2) renderer.drawPixel(cx + x, cy + y, true);
    }
  }
}

void drawButtonHintShape(const GfxRenderer& renderer, ButtonHintShape shape, int centerX, int centerY, int size) {
  const int half = std::max(4, size / 2);
  if (shape == ButtonHintShape::Back) {
    renderer.fillRect(centerX - half, centerY - half, half * 2, half * 2, true);
  } else if (shape == ButtonHintShape::Select) {
    fillCircle(renderer, centerX, centerY, half);
  } else if (shape != ButtonHintShape::None) {
    int xPoints[3] = {};
    int yPoints[3] = {};
    switch (shape) {
      case ButtonHintShape::Up:
        xPoints[0] = centerX;
        yPoints[0] = centerY - half;
        xPoints[1] = centerX - half;
        yPoints[1] = centerY + half;
        xPoints[2] = centerX + half;
        yPoints[2] = centerY + half;
        break;
      case ButtonHintShape::Down:
        xPoints[0] = centerX - half;
        yPoints[0] = centerY - half;
        xPoints[1] = centerX + half;
        yPoints[1] = centerY - half;
        xPoints[2] = centerX;
        yPoints[2] = centerY + half;
        break;
      case ButtonHintShape::Left:
        xPoints[0] = centerX - half;
        yPoints[0] = centerY;
        xPoints[1] = centerX + half;
        yPoints[1] = centerY - half;
        xPoints[2] = centerX + half;
        yPoints[2] = centerY + half;
        break;
      case ButtonHintShape::Right:
        xPoints[0] = centerX - half;
        yPoints[0] = centerY - half;
        xPoints[1] = centerX + half;
        yPoints[1] = centerY;
        xPoints[2] = centerX - half;
        yPoints[2] = centerY + half;
        break;
      default:
        return;
    }
    renderer.fillPolygon(xPoints, yPoints, 3, true);
  }
}
}  // namespace

bool LyraTheme::hasThemeIcon(UIIcon icon) const {
  return assetRoot_ != nullptr && icons_ != nullptr && icons_->find(icon) != icons_->end();
}

bool LyraTheme::drawThemeIcon(const GfxRenderer& renderer, UIIcon icon, int x, int y, int size) const {
  if (assetRoot_ == nullptr || icons_ == nullptr || !isBmpIconSize(size)) return false;
  const auto it = icons_->find(icon);
  if (it == icons_->end() || it->second.empty()) return false;

  std::string path = assetRoot_;
  if (!path.empty() && path.back() != '/') path += "/";
  path += it->second;

  HalFile file;
  if (!Storage.openFileForRead("THEME", path.c_str(), file)) return false;
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return false;

  float scale = 1.0f;
  if (bitmap.getWidth() > size) {
    scale = std::min(scale, static_cast<float>(size) / static_cast<float>(bitmap.getWidth()));
  }
  if (bitmap.getHeight() > size) {
    scale = std::min(scale, static_cast<float>(size) / static_cast<float>(bitmap.getHeight()));
  }
  const int drawnWidth = std::max(1, static_cast<int>(std::floor(bitmap.getWidth() * scale)));
  const int drawnHeight = std::max(1, static_cast<int>(std::floor(bitmap.getHeight() * scale)));
  renderer.drawBitmap(bitmap, x + (size - drawnWidth) / 2, y + (size - drawnHeight) / 2, size, size);
  return true;
}

void LyraTheme::fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const {
  const bool charging = gpio.isUsbConnected();

  if (charging) {
    // Solid fill when charging so lightning bolt is visible
    renderer.fillRect(rect.x + 2, rect.y + 2, rect.width - 5, rect.height - 4);
    drawBatteryLightningBolt(renderer, rect.x + 4, rect.y + 2);
  } else {
    if (percentage > 10) {
      renderer.fillRect(rect.x + 2, rect.y + 2, 3, rect.height - 4);
    }
    if (percentage > 40) {
      renderer.fillRect(rect.x + 6, rect.y + 2, 3, rect.height - 4);
    }
    if (percentage > 70) {
      renderer.fillRect(rect.x + 10, rect.y + 2, 3, rect.height - 4);
    }
  }
}

void LyraTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  if (header_ != nullptr && header_->enabled) {
    renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

    const bool showBatteryPercentage =
        SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
    const int batteryX = rect.x + rect.width - metrics().contentSidePadding - metrics().batteryWidth;
    drawBatteryRight(renderer,
                     Rect{batteryX, rect.y + header_->batteryOffsetY, metrics().batteryWidth, metrics().batteryHeight},
                     showBatteryPercentage);

    const auto style = header_->bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    if (title != nullptr) {
      const int reserveRight = rect.width - batteryX + metrics().batteryWidth + metrics().contentSidePadding;
      const int maxTitleWidth = std::max(0, rect.width - reserveRight * 2);
      const auto truncatedTitle = renderer.truncatedText(header_->fontId, title, maxTitleWidth, style);
      const int lineHeight = renderer.getLineHeight(header_->fontId);
      const int titleY = rect.y + header_->titleOffsetY + std::max(0, (rect.height - lineHeight) / 2);
      if (header_->centeredTitle) {
        const int textWidth = renderer.getTextWidth(header_->fontId, truncatedTitle.c_str(), style);
        renderer.drawText(header_->fontId, rect.x + (rect.width - textWidth) / 2, titleY, truncatedTitle.c_str(), true,
                          style);
      } else {
        renderer.drawText(header_->fontId, rect.x + metrics().contentSidePadding, titleY, truncatedTitle.c_str(), true,
                          style);
      }
    }

    if (subtitle != nullptr && subtitle[0] != '\0') {
      const auto truncatedSubtitle =
          renderer.truncatedText(SMALL_FONT_ID, subtitle, rect.width - metrics().contentSidePadding * 2);
      const int subtitleWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedSubtitle.c_str());
      renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - metrics().contentSidePadding - subtitleWidth,
                        rect.y + rect.height - renderer.getLineHeight(SMALL_FONT_ID) - 4, truncatedSubtitle.c_str(),
                        true);
    }

    if (header_->showDivider) {
      renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
    }
    return;
  }

  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  // Position icon at right edge, drawBatteryRight will place text to the left
  const int batteryX = rect.x + rect.width - 12 - metrics().batteryWidth;
  drawBatteryRight(renderer, Rect{batteryX, rect.y + 5, metrics().batteryWidth, metrics().batteryHeight},
                   showBatteryPercentage);

  int maxTitleWidth = title != nullptr ? renderer.getTextWidth(UI_12_FONT_ID, title, EpdFontFamily::BOLD) : 0;
  int maxSubtitleWidth =
      subtitle != nullptr ? renderer.getTextWidth(SMALL_FONT_ID, subtitle, EpdFontFamily::REGULAR) : 0;

  // Available space is the distance between the side paddings, and a with side padding between title and subtitle.
  const int availableSpace = rect.width - metrics().contentSidePadding * 3;

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
    const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int titleY =
        std::min(rect.y + metrics().batteryBarHeight + 3, rect.y + std::max(0, rect.height - titleLineHeight - 6));
    renderer.drawText(UI_12_FONT_ID, rect.x + metrics().contentSidePadding, titleY, truncatedTitle.c_str(), true,
                      EpdFontFamily::BOLD);
    renderer.drawLine(rect.x, rect.y + rect.height - 3, rect.x + rect.width - 1, rect.y + rect.height - 3, 3, true);
  }

  if (subtitle) {
    auto truncatedSubtitle = renderer.truncatedText(SMALL_FONT_ID, subtitle, maxSubtitleWidth, EpdFontFamily::REGULAR);
    int truncatedSubtitleWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedSubtitle.c_str());
    const int subtitleLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const int subtitleY = std::min(rect.y + 50, rect.y + std::max(0, rect.height - subtitleLineHeight - 6));
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - metrics().contentSidePadding - truncatedSubtitleWidth,
                      subtitleY, truncatedSubtitle.c_str(), true);
  }
}

void LyraTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  int currentX = rect.x + metrics().contentSidePadding;
  int rightSpace = metrics().contentSidePadding;
  if (rightLabel) {
    auto truncatedRightLabel =
        renderer.truncatedText(SMALL_FONT_ID, rightLabel, maxListValueWidth, EpdFontFamily::REGULAR);
    int rightLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedRightLabel.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - metrics().contentSidePadding - rightLabelWidth, rect.y + 7,
                      truncatedRightLabel.c_str());
    rightSpace += rightLabelWidth + hPaddingInSelection;
  }

  auto truncatedLabel = renderer.truncatedText(
      UI_10_FONT_ID, label, rect.width - metrics().contentSidePadding - rightSpace, EpdFontFamily::REGULAR);
  renderer.drawText(UI_10_FONT_ID, currentX, rect.y + 6, truncatedLabel.c_str(), true, EpdFontFamily::REGULAR);

  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

void LyraTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                           bool selected) const {
  if (tabBar_ != nullptr && tabBar_->enabled && tabBar_->equalWidth && !tabs.empty()) {
    const int tabCount = static_cast<int>(tabs.size());
    const int tabY = rect.y + 4;
    const int tabHeight = rect.height - 12;
    const auto style = tabBar_->bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    for (int i = 0; i < tabCount; ++i) {
      const int slotX = rect.x + (i * rect.width) / tabCount;
      const int nextSlotX = rect.x + ((i + 1) * rect.width) / tabCount;
      const int slotWidth = nextSlotX - slotX;
      const int inset = std::min(tabBar_->horizontalInset, slotWidth / 2);
      const int tabX = slotX + inset;
      const int tabWidth = std::max(0, slotWidth - inset * 2);
      const auto& tab = tabs[i];
      if (tab.selected && tabBar_->selectionStyle == ThemeMenuSelectionStyle::Fill) {
        renderer.fillRoundedRect(tabX, tabY, tabWidth, tabHeight, tabBar_->selectedCornerRadius,
                                 selected ? Color::Black : Color::LightGray);
      }
      const int textWidth = renderer.getTextWidth(tabBar_->fontId, tab.label, style);
      const int textX = tabX + (tabWidth - textWidth) / 2;
      const int textY = tabY + (tabHeight - renderer.getLineHeight(tabBar_->fontId)) / 2;
      renderer.drawText(tabBar_->fontId, textX, textY, tab.label,
                        !(tab.selected && selected && tabBar_->selectedTextInverted), style);
      if (tab.selected && tabBar_->selectionStyle == ThemeMenuSelectionStyle::Underline) {
        const int underlineY = std::min(rect.y + rect.height - 3, textY + renderer.getLineHeight(tabBar_->fontId) + 3);
        renderer.drawLine(textX, underlineY, textX + textWidth - 1, underlineY, true);
      }
    }
    if (tabBar_->drawDivider) {
      renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
    }
    return;
  }

  int currentX = rect.x + metrics().contentSidePadding;

  if (selected) {
    renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::LightGray);
  }

  for (const auto& tab : tabs) {
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, tab.label, EpdFontFamily::REGULAR);

    if (tab.selected) {
      if (selected) {
        renderer.fillRoundedRect(currentX, rect.y + 1, textWidth + 2 * hPaddingInSelection, rect.height - 4,
                                 cornerRadius, Color::Black);
      } else {
        renderer.fillRectDither(currentX, rect.y, textWidth + 2 * hPaddingInSelection, rect.height - 3,
                                Color::LightGray);
        renderer.drawLine(currentX, rect.y + rect.height - 3, currentX + textWidth + 2 * hPaddingInSelection,
                          rect.y + rect.height - 3, 2, true);
      }
    }

    renderer.drawText(UI_10_FONT_ID, currentX + hPaddingInSelection, rect.y + 6, tab.label, !(tab.selected && selected),
                      EpdFontFamily::REGULAR);

    currentX += textWidth + metrics().tabSpacing + 2 * hPaddingInSelection;
  }

  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

int LyraTheme::getListPageItems(int contentHeight, bool hasSubtitle) const {
  int rowHeight = (hasSubtitle) ? metrics().listWithSubtitleRowHeight : metrics().listRowHeight;
  return contentHeight / rowHeight;
}

void LyraTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<UIIcon(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue, bool highlightValue,
                         const std::function<bool(int index)>& rowDimmed) const {
  if (list_ != nullptr && list_->enabled) {
    const ThemeListSpec& spec = *list_;
    const bool hasSubtitle = rowSubtitle != nullptr;
    int rowHeight = hasSubtitle ? metrics().listWithSubtitleRowHeight : metrics().listRowHeight;
    if (hasSubtitle && spec.subtitleRowAutoHeight) {
      rowHeight = spec.subtitleTopPadding + renderer.getLineHeight(spec.fontId) + spec.subtitleInterLineGap +
                  renderer.getLineHeight(spec.subtitleFontId) + spec.subtitleBottomPadding;
    }
    const int rowStep = rowHeight + std::max(0, spec.rowGap);
    const int pageItems = std::max(1, rect.height / std::max(1, rowStep));
    const int totalPages = (itemCount + pageItems - 1) / pageItems;
    const int contentWidth =
        rect.width - (totalPages > 1 ? (metrics().scrollBarWidth + metrics().scrollBarRightOffset) : 1);

    if (totalPages > 1) {
      const int scrollAreaHeight = rect.height;
      const int scrollBarHeight = std::max(metrics().scrollBarWidth, (scrollAreaHeight * pageItems) / itemCount);
      const int currentPage = selectedIndex / pageItems;
      const int scrollBarY = rect.y + ((scrollAreaHeight - scrollBarHeight) * currentPage) / (totalPages - 1);
      const int scrollBarX = rect.x + rect.width - metrics().scrollBarRightOffset;
      renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + scrollAreaHeight, true);
      renderer.fillRect(scrollBarX - metrics().scrollBarWidth, scrollBarY, metrics().scrollBarWidth, scrollBarHeight,
                        true);
    }

    if (selectedIndex >= 0 && !spec.rowBackgrounds) {
      const int selectedY = rect.y + selectedIndex % pageItems * rowStep;
      Rect selectionRect{rect.x + metrics().contentSidePadding + spec.selectionInsetX, selectedY + spec.selectionInsetY,
                         contentWidth - metrics().contentSidePadding * 2 - spec.selectionInsetX * 2,
                         rowHeight - spec.selectionInsetY * 2};
      if (spec.selectionStyle == ThemeMenuSelectionStyle::Fill && spec.selectionFill) {
        renderer.fillRoundedRect(selectionRect.x, selectionRect.y, selectionRect.width, selectionRect.height,
                                 spec.selectionCornerRadius, Color::LightGray);
      }
      if (spec.selectionStyle == ThemeMenuSelectionStyle::Outline || spec.selectionOutline) {
        renderer.drawRoundedRect(selectionRect.x, selectionRect.y, selectionRect.width, selectionRect.height, 1,
                                 spec.selectionCornerRadius, true);
      }
    }

    const int rowX = rect.x + spec.rowSidePadding;
    const int rowWidth = contentWidth - spec.rowSidePadding * 2;
    int textX =
        spec.rowBackgrounds ? rowX + spec.textInsetX : rect.x + metrics().contentSidePadding + hPaddingInSelection;
    int textWidth = spec.rowBackgrounds ? rowWidth - spec.textInsetX * 2
                                        : contentWidth - metrics().contentSidePadding * 2 - hPaddingInSelection * 2;
    const int iconSize =
        spec.iconSize > 0 ? spec.iconSize : ((rowSubtitle != nullptr) ? mainMenuIconSize : listIconSize);
    if (rowIcon != nullptr && spec.showIcons) {
      textX += iconSize + spec.textGap;
      textWidth -= iconSize + spec.textGap;
    }

    const auto pageStartIndex = selectedIndex / pageItems * pageItems;
    const auto titleStyle = spec.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
      const int itemY = rect.y + (i % pageItems) * rowStep;
      const bool selected = i == selectedIndex;
      int rowTextWidth = textWidth;
      if (spec.rowBackgrounds) {
        renderer.fillRoundedRect(rowX, itemY, rowWidth, rowHeight, spec.selectionCornerRadius,
                                 selected ? Color::Black : Color::White);
      }

      int valueWidth = 0;
      std::string valueText;
      if (rowValue != nullptr) {
        valueText = rowValue(i);
        valueText = renderer.truncatedText(spec.valueFontId, valueText.c_str(), maxListValueWidth);
        valueWidth = renderer.getTextWidth(spec.valueFontId, valueText.c_str()) + hPaddingInSelection;
        rowTextWidth -= valueWidth;
      }

      const auto itemName = rowTitle(i);
      const auto item = renderer.truncatedText(spec.fontId, itemName.c_str(), rowTextWidth, titleStyle);
      bool centerSingleLine = spec.centerSingleLineRows && rowSubtitle == nullptr;
      std::string subtitleText;
      if (rowSubtitle != nullptr) {
        subtitleText = rowSubtitle(i);
        centerSingleLine = spec.centerSingleLineRows && subtitleText.empty();
      }
      const int titleY = centerSingleLine
                             ? itemY + (rowHeight - renderer.getLineHeight(spec.fontId)) / 2
                             : itemY + (spec.subtitleRowAutoHeight && rowSubtitle != nullptr ? spec.subtitleTopPadding
                                                                                             : spec.titleOffsetY);
      renderer.drawText(spec.fontId, textX, titleY, item.c_str(), !(selected && spec.selectedTextInverted), titleStyle);
      if (selected && spec.selectionStyle == ThemeMenuSelectionStyle::Underline) {
        const int underlineWidth = renderer.getTextWidth(spec.fontId, item.c_str(), titleStyle);
        const int underlineY = std::min(itemY + rowHeight - 4, titleY + renderer.getLineHeight(spec.fontId) + 2);
        renderer.drawLine(textX, underlineY, textX + underlineWidth - 1, underlineY, 1, true);
      }

      if (rowDimmed && rowDimmed(i) && !selected) {
        const int titleWidth = renderer.getTextWidth(spec.fontId, item.c_str(), titleStyle);
        const int lineH = renderer.getLineHeight(spec.fontId);
        for (int py = titleY; py < titleY + lineH; py++)
          for (int px = textX; px < textX + titleWidth; px++)
            if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
      }

      if (rowIcon != nullptr && spec.showIcons) {
        const UIIcon icon = rowIcon(i);
        const int titleLineHeight = renderer.getLineHeight(spec.fontId);
        const int textBlockTop = spec.titleOffsetY;
        const int textBlockBottom = rowSubtitle != nullptr
                                        ? spec.subtitleOffsetY + renderer.getLineHeight(spec.subtitleFontId)
                                        : spec.titleOffsetY + titleLineHeight;
        const int iconY = itemY + (textBlockTop + textBlockBottom - iconSize) / 2 + spec.iconOffsetY;
        const int iconX =
            spec.rowBackgrounds ? rowX + spec.textInsetX : rect.x + metrics().contentSidePadding + hPaddingInSelection;
        if (!drawThemeIcon(renderer, icon, iconX, iconY, iconSize)) {
          const uint8_t* iconBitmap = iconForName(icon, iconSize);
          if (iconBitmap != nullptr) {
            renderer.drawIcon(iconBitmap, iconX, iconY, iconSize, iconSize);
          }
        }
      }

      if (rowSubtitle != nullptr && !subtitleText.empty()) {
        const auto subtitle = renderer.truncatedText(spec.subtitleFontId, subtitleText.c_str(), rowTextWidth);
        const int subtitleY = spec.subtitleRowAutoHeight
                                  ? titleY + renderer.getLineHeight(spec.fontId) + spec.subtitleInterLineGap
                                  : itemY + spec.subtitleOffsetY;
        renderer.drawText(spec.subtitleFontId, textX, subtitleY, subtitle.c_str(),
                          !(selected && spec.selectedTextInverted));
      }

      if (!valueText.empty()) {
        if (selected && highlightValue) {
          renderer.fillRoundedRect(
              rect.x + contentWidth - metrics().contentSidePadding - hPaddingInSelection - valueWidth, itemY,
              valueWidth + hPaddingInSelection, rowHeight, spec.selectionCornerRadius, Color::Black);
        }
        const int valueY = (centerSingleLine || spec.centerValueVertically)
                               ? itemY + (rowHeight - renderer.getLineHeight(spec.valueFontId)) / 2
                               : itemY + (rowSubtitle != nullptr ? spec.subtitleValueOffsetY : spec.valueOffsetY);
        const int valueX = spec.rowBackgrounds ? rowX + rowWidth - spec.textInsetX - valueWidth
                                               : rect.x + contentWidth - metrics().contentSidePadding - valueWidth;
        renderer.drawText(spec.valueFontId, valueX, valueY, valueText.c_str(),
                          !(selected && (highlightValue || (spec.rowBackgrounds && spec.selectedTextInverted))));
      }
    }
    return;
  }

  int rowHeight = (rowSubtitle != nullptr) ? metrics().listWithSubtitleRowHeight : metrics().listRowHeight;
  int pageItems = rect.height / rowHeight;

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    const int scrollAreaHeight = rect.height;

    // Draw scroll bar
    const int scrollBarHeight = (scrollAreaHeight * pageItems) / itemCount;
    const int currentPage = selectedIndex / pageItems;
    const int scrollBarY = rect.y + ((scrollAreaHeight - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - metrics().scrollBarRightOffset;
    renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + scrollAreaHeight, true);
    renderer.fillRect(scrollBarX - metrics().scrollBarWidth, scrollBarY, metrics().scrollBarWidth, scrollBarHeight,
                      true);
  }

  // Draw selection
  int contentWidth = rect.width - (totalPages > 1 ? (metrics().scrollBarWidth + metrics().scrollBarRightOffset) : 1);
  if (selectedIndex >= 0) {
    renderer.fillRoundedRect(rect.x + metrics().contentSidePadding, rect.y + selectedIndex % pageItems * rowHeight,
                             contentWidth - metrics().contentSidePadding * 2, rowHeight, cornerRadius,
                             Color::LightGray);
  }

  int textX = rect.x + metrics().contentSidePadding + hPaddingInSelection;
  int textWidth = contentWidth - metrics().contentSidePadding * 2 - hPaddingInSelection * 2;
  int iconSize;
  if (rowIcon != nullptr) {
    iconSize = (rowSubtitle != nullptr) ? mainMenuIconSize : listIconSize;
    textX += iconSize + hPaddingInSelection;
    textWidth -= iconSize + hPaddingInSelection;
  }

  // Draw all items
  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
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

    // Apply checkerboard dither to create gray text effect for dimmed items
    if (rowDimmed && rowDimmed(i) && i != selectedIndex) {
      const int titleWidth = renderer.getTextWidth(UI_10_FONT_ID, item.c_str());
      const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
      for (int py = itemY + 7; py < itemY + 7 + lineH; py++)
        for (int px = textX; px < textX + titleWidth; px++)
          if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
    }

    if (rowIcon != nullptr) {
      UIIcon icon = rowIcon(i);
      const int titleLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
      const int textBlockTop = 7;
      const int textBlockBottom =
          rowSubtitle != nullptr ? 30 + renderer.getLineHeight(SMALL_FONT_ID) : textBlockTop + titleLineHeight;
      const int iconY = itemY + (textBlockTop + textBlockBottom - iconSize) / 2;
      if (!drawThemeIcon(renderer, icon, rect.x + metrics().contentSidePadding + hPaddingInSelection, iconY,
                         iconSize)) {
        const uint8_t* iconBitmap = iconForName(icon, iconSize);
        if (iconBitmap != nullptr) {
          renderer.drawIcon(iconBitmap, rect.x + metrics().contentSidePadding + hPaddingInSelection, iconY, iconSize,
                            iconSize);
        }
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
        renderer.fillRoundedRect(
            rect.x + contentWidth - metrics().contentSidePadding - hPaddingInSelection - valueWidth, itemY,
            valueWidth + hPaddingInSelection, rowHeight, cornerRadius, Color::Black);
      }

      int valueY = itemY + 6;
      if (rowSubtitle != nullptr) {
        valueY = itemY + 16;
      }
      renderer.drawText(UI_10_FONT_ID, rect.x + contentWidth - metrics().contentSidePadding - valueWidth, valueY,
                        valueText.c_str(), !(i == selectedIndex && highlightValue));
    }
  }
}

void LyraTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) const {
  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  const int pageWidth = renderer.getScreenWidth();
  const int buttonWidth = buttonHints_ != nullptr && buttonHints_->enabled ? buttonHints_->buttonWidth : 80;
  const int smallButtonHeight = buttonHints_ != nullptr && buttonHints_->enabled ? buttonHints_->smallButtonHeight : 15;
  const int buttonHeight = metrics().buttonHintsHeight;
  const int buttonY = metrics().buttonHintsHeight;  // Distance from bottom
  const int textYOffset = buttonHints_ != nullptr && buttonHints_->enabled ? buttonHints_->textOffsetY : 7;
  const int buttonCornerRadius =
      buttonHints_ != nullptr && buttonHints_->enabled ? buttonHints_->cornerRadius : cornerRadius;
  const int fontId = buttonHints_ != nullptr && buttonHints_->enabled ? buttonHints_->fontId : SMALL_FONT_ID;
  const auto style = buttonHints_ != nullptr && buttonHints_->enabled && buttonHints_->bold ? EpdFontFamily::BOLD
                                                                                            : EpdFontFamily::REGULAR;
  const ThemeButtonHintsStyle hintStyle =
      buttonHints_ != nullptr && buttonHints_->enabled ? buttonHints_->style : ThemeButtonHintsStyle::Buttons;
  const bool shapes = hintStyle == ThemeButtonHintsStyle::Shapes;
  const int shapeSize = buttonHints_ != nullptr && buttonHints_->enabled ? buttonHints_->shapeSize : 18;
  if (hintStyle == ThemeButtonHintsStyle::Groups) {
    const int sidePadding = buttonHints_->sidePadding;
    const int groupGap = buttonHints_->groupGap;
    const int bottomMargin = buttonHints_->bottomMargin;
    const int innerPadding = buttonHints_->innerPadding;
    const int hintHeight = std::max(1, metrics().buttonHintsHeight - bottomMargin);
    const int groupWidth = std::max(1, (pageWidth - sidePadding * 2 - groupGap) / 2);
    const int outlineY = pageHeight - hintHeight - bottomMargin;
    const int leftGroupX = sidePadding;
    const int rightGroupX = leftGroupX + groupWidth + groupGap;
    const char* labels[] = {btn1, btn2, btn3, btn4};
    const int selectWidth = labels[1] != nullptr ? renderer.getTextWidth(fontId, labels[1], style) : 0;
    const int downWidth = labels[3] != nullptr ? renderer.getTextWidth(fontId, labels[3], style) : 0;

    renderer.fillRect(leftGroupX, outlineY, groupWidth, hintHeight, false);
    renderer.fillRect(rightGroupX, outlineY, groupWidth, hintHeight, false);
    renderer.drawRoundedRect(leftGroupX, outlineY, groupWidth, hintHeight, 2, buttonCornerRadius, true);
    renderer.drawRoundedRect(rightGroupX, outlineY, groupWidth, hintHeight, 2, buttonCornerRadius, true);

    const int textY = outlineY + (hintHeight - renderer.getLineHeight(fontId)) / 2;
    if (labels[0] != nullptr && labels[0][0] != '\0') {
      renderer.drawText(fontId, leftGroupX + innerPadding, textY, labels[0], true, style);
    }
    if (labels[1] != nullptr && labels[1][0] != '\0') {
      renderer.drawText(fontId, leftGroupX + groupWidth - innerPadding - selectWidth, textY, labels[1], true, style);
    }
    if (labels[2] != nullptr && labels[2][0] != '\0') {
      renderer.drawText(fontId, rightGroupX + innerPadding, textY, labels[2], true, style);
    }
    if (labels[3] != nullptr && labels[3][0] != '\0') {
      renderer.drawText(fontId, rightGroupX + groupWidth - innerPadding - downWidth, textY, labels[3], true, style);
    }
    renderer.setOrientation(orig_orientation);
    return;
  }
  // X3 has wider screen in portrait (528 vs 480), use more spacing
  constexpr int x4ButtonPositions[] = {58, 146, 254, 342};
  constexpr int x3ButtonPositions[] = {65, 157, 291, 383};
  const int* buttonPositions = gpio.deviceIsX3() ? x3ButtonPositions : x4ButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};

  for (int i = 0; i < 4; i++) {
    const int x = buttonPositions[i];
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      if (shapes) {
        drawButtonHintShape(renderer, shapeForButtonHintLabel(labels[i]), x + buttonWidth / 2,
                            pageHeight - buttonY + buttonHeight / 2, shapeSize);
        continue;
      }
      if (buttonHints_ == nullptr || !buttonHints_->enabled || buttonHints_->fill) {
        renderer.fillRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, buttonCornerRadius, Color::White);
      }
      if (buttonHints_ == nullptr || !buttonHints_->enabled || buttonHints_->outline) {
        renderer.drawRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, 1, buttonCornerRadius, true, true,
                                 false, false, true);
      }
      const int textWidth = renderer.getTextWidth(fontId, labels[i], style);
      const int textX = x + (buttonWidth - 1 - textWidth) / 2;
      renderer.drawText(fontId, textX, pageHeight - buttonY + textYOffset, labels[i], true, style);
    } else if (buttonHints_ == nullptr || !buttonHints_->enabled || buttonHints_->drawEmpty) {
      if (buttonHints_ == nullptr || !buttonHints_->enabled || buttonHints_->fill) {
        renderer.fillRoundedRect(x, pageHeight - smallButtonHeight, buttonWidth, smallButtonHeight, buttonCornerRadius,
                                 Color::White);
      }
      if (buttonHints_ == nullptr || !buttonHints_->enabled || buttonHints_->outline) {
        renderer.drawRoundedRect(x, pageHeight - smallButtonHeight, buttonWidth, smallButtonHeight, 1,
                                 buttonCornerRadius, true, true, false, false, true);
      }
    }
  }

  renderer.setOrientation(orig_orientation);
}

void LyraTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  const int screenWidth = renderer.getScreenWidth();
  const int buttonWidth = metrics().sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 78;                         // Height on screen (width when rotated)
  constexpr int buttonMargin = 0;
  const int buttonCornerRadius =
      buttonHints_ != nullptr && buttonHints_->enabled ? buttonHints_->cornerRadius : cornerRadius;
  const int fontId = buttonHints_ != nullptr && buttonHints_->enabled ? buttonHints_->fontId : SMALL_FONT_ID;
  const auto style = buttonHints_ != nullptr && buttonHints_->enabled && buttonHints_->bold ? EpdFontFamily::BOLD
                                                                                            : EpdFontFamily::REGULAR;
  const ThemeButtonHintsStyle hintStyle =
      buttonHints_ != nullptr && buttonHints_->enabled ? buttonHints_->style : ThemeButtonHintsStyle::Buttons;
  const bool shapes = hintStyle == ThemeButtonHintsStyle::Shapes;
  const int shapeSize = buttonHints_ != nullptr && buttonHints_->enabled ? buttonHints_->shapeSize : 18;

  auto drawSideHint = [&](const int x, const int y, const char* label, const bool leftOpen, const bool rightOpen) {
    if (label == nullptr || label[0] == '\0') return;

    if (shapes) {
      ButtonHintShape shape = shapeForButtonHintLabel(label);
      if (strcmp(label, ">") == 0 || matchesLabel(label, tr(STR_DIR_RIGHT))) {
        shape = ButtonHintShape::Up;
      } else if (strcmp(label, "<") == 0 || matchesLabel(label, tr(STR_DIR_LEFT))) {
        shape = ButtonHintShape::Down;
      }
      drawButtonHintShape(renderer, shape, x + buttonWidth / 2, y + buttonHeight / 2, shapeSize);
      return;
    }

    if (buttonHints_ == nullptr || !buttonHints_->enabled || buttonHints_->fill) {
      renderer.fillRoundedRect(x, y, buttonWidth, buttonHeight, buttonCornerRadius, Color::White);
    }
    if (buttonHints_ == nullptr || !buttonHints_->enabled || buttonHints_->outline) {
      renderer.drawRoundedRect(x, y, buttonWidth, buttonHeight, 1, buttonCornerRadius, leftOpen, rightOpen, leftOpen,
                               rightOpen, true);
    }
    const int textWidth = renderer.getTextWidth(fontId, label, style);
    const int textHeight = renderer.getTextHeight(fontId);
    const int textX = x + (buttonWidth - textHeight) / 2;
    renderer.drawTextRotated90CW(fontId, textX, y + (buttonHeight + textWidth) / 2, label, true, style);
  };

  if (gpio.deviceIsX3()) {
    // X3 layout: Up on left side, Down on right side, positioned higher
    constexpr int x3ButtonY = 155;

    drawSideHint(buttonMargin, x3ButtonY, topBtn, false, true);
    drawSideHint(screenWidth - buttonWidth, x3ButtonY, bottomBtn, true, false);
  } else {
    // X4 layout: Both buttons stacked on right side
    const char* labels[] = {topBtn, bottomBtn};
    const int x = screenWidth - buttonWidth;

    for (int i = 0; i < 2; i++) {
      const int y = topHintButtonY + (i * buttonHeight) + 5;
      drawSideHint(x, y, labels[i], true, false);
    }
  }
}

void LyraTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, std::function<bool()> storeCoverBuffer,
                                    bool coverStripSelected) const {
  if (homeRecents_ != nullptr && homeRecents_->type == ThemeHomeRecentsType::CoverStrip) {
    drawCoverStripRecents(renderer, rect, recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          storeCoverBuffer, coverStripSelected);
    return;
  }
  if (homeRecents_ != nullptr && homeRecents_->type == ThemeHomeRecentsType::None) {
    coverBufferStored = false;
    coverRendered = false;
    bufferRestored = false;
    return;
  }

  const int tileWidth = rect.width - 2 * metrics().contentSidePadding;
  const int tileHeight = rect.height;
  const int tileY = rect.y;
  const bool hasContinueReading = !recentBooks.empty();
  if (coverWidth == 0) {
    coverWidth = metrics().homeCoverHeight * 0.6;
  }

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    RecentBook book = recentBooks[0];
    if (!coverRendered) {
      std::string coverPath = book.coverBmpPath;
      bool hasCover = true;
      int tileX = metrics().contentSidePadding;
      if (coverPath.empty()) {
        hasCover = false;
      } else {
        const std::string coverBmpPath = UITheme::getCoverThumbPath(coverPath, metrics().homeCoverHeight);

        // First time: load cover from SD and render
        HalFile file;
        if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
          Bitmap bitmap(file);
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            coverWidth = bitmap.getWidth();
            renderer.drawBitmap(bitmap, tileX + hPaddingInSelection, tileY + hPaddingInSelection, coverWidth,
                                metrics().homeCoverHeight);
          } else {
            hasCover = false;
          }
          file.close();
        }
      }

      // Draw either way
      renderer.drawRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection, coverWidth, metrics().homeCoverHeight,
                        true);

      if (!hasCover) {
        // Render empty cover
        renderer.fillRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection + (metrics().homeCoverHeight / 3),
                          coverWidth, 2 * metrics().homeCoverHeight / 3, true);
        renderer.drawIcon(CoverIcon, tileX + hPaddingInSelection + 24, tileY + hPaddingInSelection + 24, 32, 32);
      }

      coverBufferStored = storeCoverBuffer();
      coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer
    }

    bool bookSelected = (selectorIndex == 0);

    int tileX = metrics().contentSidePadding;
    int textWidth = tileWidth - 2 * hPaddingInSelection - metrics().verticalSpacing - coverWidth;

    if (bookSelected) {
      // Draw selection box
      renderer.fillRoundedRect(tileX, tileY, tileWidth, hPaddingInSelection, cornerRadius, true, true, false, false,
                               Color::LightGray);
      renderer.fillRectDither(tileX, tileY + hPaddingInSelection, hPaddingInSelection, metrics().homeCoverHeight,
                              Color::LightGray);
      renderer.fillRectDither(tileX + hPaddingInSelection + coverWidth, tileY + hPaddingInSelection,
                              tileWidth - hPaddingInSelection - coverWidth, metrics().homeCoverHeight,
                              Color::LightGray);
      renderer.fillRoundedRect(tileX, tileY + metrics().homeCoverHeight + hPaddingInSelection, tileWidth,
                               hPaddingInSelection, cornerRadius, false, false, true, true, Color::LightGray);
    }

    auto titleLines = renderer.wrappedText(UI_12_FONT_ID, book.title.c_str(), textWidth, 3, EpdFontFamily::BOLD);

    auto author = renderer.truncatedText(UI_10_FONT_ID, book.author.c_str(), textWidth);
    const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int titleBlockHeight = titleLineHeight * static_cast<int>(titleLines.size());
    const int authorHeight = book.author.empty() ? 0 : (renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2);
    const int totalBlockHeight = titleBlockHeight + authorHeight;
    int titleY = tileY + tileHeight / 2 - totalBlockHeight / 2;
    const int textX = tileX + hPaddingInSelection + coverWidth + metrics().verticalSpacing;
    for (const auto& line : titleLines) {
      renderer.drawText(UI_12_FONT_ID, textX, titleY, line.c_str(), true, EpdFontFamily::BOLD);
      titleY += titleLineHeight;
    }
    if (!book.author.empty()) {
      titleY += renderer.getLineHeight(UI_10_FONT_ID) / 2;
      renderer.drawText(UI_10_FONT_ID, textX, titleY, author.c_str(), true);
    }
  } else {
    drawEmptyRecents(renderer, rect);
  }
}

void LyraTheme::drawCoverStripRecents(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                      const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                      bool bufferRestored, std::function<bool()> storeCoverBuffer,
                                      bool coverStripSelected) const {
  if (bufferRestored && coverRendered) {
    return;
  }

  if (recentBooks.empty()) {
    drawEmptyRecents(renderer, rect);
    coverBufferStored = false;
    coverRendered = false;
    return;
  }

  const ThemeHomeRecentsSpec& spec = *homeRecents_;
  if (spec.slots.empty()) {
    coverBufferStored = false;
    coverRendered = false;
    return;
  }

  const int bookCount = static_cast<int>(recentBooks.size());
  const int selected = selectorIndex >= 0 && selectorIndex < bookCount ? selectorIndex : 0;
  const auto& m = metrics();

  if (spec.drawPanel) {
    const int inset = std::max(0, spec.panelInsetX);
    renderer.fillRoundedRect(rect.x + inset, rect.y, std::max(0, rect.width - inset * 2), rect.height,
                             spec.panelCornerRadius, Color::LightGray);
  }

  auto resolveBookIndex = [&](const ThemeCoverSlotSpec& slot) {
    switch (slot.book) {
      case ThemeBookRef::Previous:
        if (bookCount <= 1) return -1;
        return spec.wrap ? (selected + bookCount - 1) % bookCount : selected - 1;
      case ThemeBookRef::Next:
        if (bookCount <= 1) return -1;
        return spec.wrap ? (selected + 1) % bookCount : selected + 1;
      case ThemeBookRef::Index:
        return slot.bookIndex >= 0 && slot.bookIndex < bookCount ? slot.bookIndex : -1;
      case ThemeBookRef::Selected:
      default:
        return selected;
    }
  };

  auto drawCover = [&](int bookIndex, int x, int y, int w, int h, bool selectedCover) {
    bool hasCover = bookIndex >= 0 && bookIndex < bookCount && !recentBooks[bookIndex].coverBmpPath.empty();
    if (hasCover) {
      auto drawThumb = [&](int thumbHeight) {
        const std::string coverBmpPath = UITheme::getCoverThumbPath(recentBooks[bookIndex].coverBmpPath, thumbHeight);
        HalFile file;
        if (!Storage.openFileForRead("HOME", coverBmpPath, file)) return false;
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          float cropX = 0.0f;
          float cropY = 0.0f;
          const float bitmapAspect = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
          const float targetAspect = static_cast<float>(w) / static_cast<float>(h);
          if (bitmapAspect > targetAspect) {
            cropX = std::max(0.0f, 1.0f - targetAspect / bitmapAspect);
          } else if (bitmapAspect < targetAspect) {
            cropY = std::max(0.0f, 1.0f - bitmapAspect / targetAspect);
          }
          renderer.drawBitmap(bitmap, x, y, w, h, cropX, cropY);
          return true;
        } else {
          return false;
        }
      };

      bool drawn = drawThumb(h);
      if (!drawn && h != m.homeCoverHeight) {
        drawn = drawThumb(m.homeCoverHeight);
      }
      if (!drawn) {
        hasCover = false;
      }
    }

    if (!hasCover) {
      renderer.drawRect(x, y, w, h, true);
      renderer.fillRect(x, y + h / 3, w, 2 * h / 3, true);
      renderer.drawIcon(CoverIcon, x + std::max(4, (w - 32) / 2), y + 16, 32, 32);
    }

    renderer.drawRect(x, y, w, h, true);
    if (selectedCover) {
      const int inactiveWidth =
          spec.inactiveSelectionLineWidth > 0 ? spec.inactiveSelectionLineWidth : spec.selectionLineWidth;
      const int lineWidth = std::max(1, coverStripSelected ? spec.selectionLineWidth : inactiveWidth);
      for (int i = 0; i < lineWidth; ++i) {
        renderer.drawRoundedRect(x - 6 - i, y - 6 - i, w + 12 + 2 * i, h + 12 + 2 * i, lineWidth,
                                 spec.selectionCornerRadius, true);
      }
    }
  };

  for (const auto& slot : spec.slots) {
    const int bookIndex = resolveBookIndex(slot);
    if (bookIndex < 0 || bookIndex >= bookCount) continue;

    const int h = std::min(slot.height, rect.height);
    const int w = std::max(1, h * std::max(1, slot.widthPercent) / 100);
    int x = rect.x + (rect.width - w) / 2;
    if (slot.x == ThemeSlotX::Padding) {
      x = rect.x + m.contentSidePadding;
    } else if (slot.x == ThemeSlotX::RightPadding) {
      x = rect.x + rect.width - m.contentSidePadding - w;
    }
    x += slot.xOffset;

    int y = rect.y;
    if (slot.y == ThemeSlotY::Center) {
      y = rect.y + (rect.height - h) / 2;
    }
    y += slot.yOffset;

    const bool selectedCover = slot.selected && (slot.book != ThemeBookRef::Index || bookIndex == selected);
    drawCover(bookIndex, x, y, w, h, selectedCover);

    if (slot.title.enabled) {
      const int maxWidth = std::max(40, w + 28);
      const auto style = slot.title.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      const auto titleLines = renderer.wrappedText(slot.title.fontId, recentBooks[bookIndex].title.c_str(), maxWidth,
                                                   slot.title.maxLines, style);
      int titleY = y + h + slot.title.offsetY;
      for (const auto& line : titleLines) {
        const int textWidth = renderer.getTextWidth(slot.title.fontId, line.c_str(), style);
        renderer.drawText(slot.title.fontId, x + (w - textWidth) / 2, titleY, line.c_str(), true, style);
        titleY += renderer.getLineHeight(slot.title.fontId);
      }
    }
  }

  coverBufferStored = storeCoverBuffer();
  coverRendered = coverBufferStored;
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
  if (buttonMenu_ != nullptr && buttonMenu_->enabled) {
    const auto& spec = *buttonMenu_;
    const auto& m = metrics();
    const int panelWidth = spec.panelWidth > 0 ? std::min(spec.panelWidth, rect.width) : rect.width;
    const int panelX = rect.x + (rect.width - panelWidth) / 2;
    const int panelHeight = buttonCount * m.menuRowHeight + std::max(0, buttonCount - 1) * m.menuSpacing;
    const int panelY =
        spec.centerVertically && panelHeight < rect.height ? rect.y + (rect.height - panelHeight) / 2 : rect.y;

    if (spec.drawPanel) {
      renderer.drawRoundedRect(panelX, panelY, panelWidth, panelHeight, 1, spec.panelCornerRadius, true);
    }

    for (int i = 0; i < buttonCount; ++i) {
      std::string labelStr = buttonLabel(i);
      const char* label = labelStr.c_str();
      const auto style = spec.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      Rect tileRect = Rect{panelX + spec.selectionInset, panelY + i * (m.menuRowHeight + m.menuSpacing),
                           panelWidth - spec.selectionInset * 2, m.menuRowHeight};
      if (spec.selectionStyle == ThemeMenuSelectionStyle::Pill) {
        const int maxLabelWidth = std::max(0, panelWidth - spec.selectionInset * 2 - spec.rowPaddingX);
        labelStr = renderer.truncatedText(spec.fontId, label, maxLabelWidth, style);
        label = labelStr.c_str();
        tileRect.width = std::min(tileRect.width, renderer.getTextWidth(spec.fontId, label, style) + spec.rowPaddingX);
      }
      const bool selected = selectedIndex == i;

      if (spec.selectionStyle == ThemeMenuSelectionStyle::Pill) {
        renderer.fillRoundedRect(tileRect.x, tileRect.y, tileRect.width, tileRect.height, spec.selectionCornerRadius,
                                 selected ? Color::Black : Color::White);
      } else if (selected) {
        if (spec.selectionStyle == ThemeMenuSelectionStyle::Outline) {
          renderer.drawRoundedRect(tileRect.x, tileRect.y, tileRect.width, tileRect.height, 1,
                                   spec.selectionCornerRadius, true);
        } else if (spec.selectionStyle == ThemeMenuSelectionStyle::Triangle) {
          constexpr int triangleWidth = 12;
          constexpr int triangleHeight = 18;
          const int triangleX = panelX + spec.selectionInset;
          const int triangleCenterY = tileRect.y + tileRect.height / 2;
          const int triangleXPoints[3] = {triangleX, triangleX, triangleX + triangleWidth};
          const int triangleYPoints[3] = {triangleCenterY - triangleHeight / 2, triangleCenterY + triangleHeight / 2,
                                          triangleCenterY};
          renderer.fillPolygon(triangleXPoints, triangleYPoints, 3, true);
        } else if (spec.selectionStyle == ThemeMenuSelectionStyle::Underline) {
          // Drawn after text layout below.
        } else {
          renderer.fillRoundedRect(tileRect.x, tileRect.y, tileRect.width, tileRect.height, spec.selectionCornerRadius,
                                   spec.selectionFillBlack ? Color::Black : Color::LightGray);
        }
      }

      const int lineHeight = renderer.getLineHeight(spec.fontId);
      const int textY = tileRect.y + (tileRect.height - lineHeight) / 2;
      int textX = tileRect.x + spec.textInsetX;

      if (spec.showIcons && rowIcon != nullptr) {
        UIIcon icon = rowIcon(i);
        const int iconY = tileRect.y + (tileRect.height - mainMenuIconSize) / 2;
        if (!drawThemeIcon(renderer, icon, textX, iconY, mainMenuIconSize)) {
          const uint8_t* iconBitmap = iconForName(icon, mainMenuIconSize);
          if (iconBitmap != nullptr) {
            renderer.drawIcon(iconBitmap, textX, iconY, mainMenuIconSize, mainMenuIconSize);
          }
        }
        if (hasThemeIcon(icon) || iconForName(icon, mainMenuIconSize) != nullptr) {
          textX += mainMenuIconSize + hPaddingInSelection + 2;
        }
      }

      if (spec.centeredText) {
        const int textWidth = renderer.getTextWidth(spec.fontId, label, style);
        textX = tileRect.x + (tileRect.width - textWidth) / 2;
      }
      renderer.drawText(
          spec.fontId, textX, textY, label,
          !(selected && (spec.selectedTextInverted || spec.selectionStyle == ThemeMenuSelectionStyle::Pill)), style);
      if (selected && spec.selectionStyle == ThemeMenuSelectionStyle::Underline) {
        const int textWidth = renderer.getTextWidth(spec.fontId, label, style);
        const int underlineY = std::min(tileRect.y + tileRect.height - 5, textY + lineHeight + 2);
        renderer.drawLine(textX, underlineY, textX + textWidth - 1, underlineY, 1, true);
      }
    }
    return;
  }

  for (int i = 0; i < buttonCount; ++i) {
    int tileWidth = rect.width - metrics().contentSidePadding * 2;
    Rect tileRect =
        Rect{rect.x + metrics().contentSidePadding, rect.y + i * (metrics().menuRowHeight + metrics().menuSpacing),
             tileWidth, metrics().menuRowHeight};

    const bool selected = selectedIndex == i;

    if (selected) {
      renderer.fillRoundedRect(tileRect.x, tileRect.y, tileRect.width, tileRect.height, cornerRadius, Color::LightGray);
    }

    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    int textX = tileRect.x + 16;
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int textY = tileRect.y + (metrics().menuRowHeight - lineHeight) / 2;

    if (rowIcon != nullptr) {
      UIIcon icon = rowIcon(i);
      const int iconY = tileRect.y + (tileRect.height - mainMenuIconSize) / 2;
      if (!drawThemeIcon(renderer, icon, textX, iconY, mainMenuIconSize)) {
        const uint8_t* iconBitmap = iconForName(icon, mainMenuIconSize);
        if (iconBitmap != nullptr) {
          renderer.drawIcon(iconBitmap, textX, iconY, mainMenuIconSize, mainMenuIconSize);
        }
      }
      if (hasThemeIcon(icon) || iconForName(icon, mainMenuIconSize) != nullptr) {
        textX += mainMenuIconSize + hPaddingInSelection + 2;
      }
    }

    renderer.drawText(UI_12_FONT_ID, textX, textY, label, true);
  }
}
