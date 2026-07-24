#include "CrossViTheme.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "activities/home/DashboardProgress.h"
#include "activities/home/HomeBookSummary.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/cover.h"
#include "components/icons/folder.h"
#include "components/icons/hotspot.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/transfer.h"
#include "components/icons/wifi.h"
#include "components/themes/HomeMenuLayout.h"
#include "components/themes/crossvi/CrossViLayout.h"
#include "fontIds.h"

namespace {

constexpr int CORNER_RADIUS = 6;
constexpr int MENU_COLUMNS = 2;
constexpr int MENU_GAP = 8;
constexpr int MENU_SIDE_PADDING = 20;
constexpr int MENU_ICON_SIZE = 32;
constexpr int MENU_CONTENT_PADDING = 18;

const uint8_t* iconForName(const UIIcon icon) {
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

std::string formatDuration(const uint32_t seconds) {
  char value[32]{};
  const uint32_t minutes = seconds == 0 ? 0 : std::max<uint32_t>(1, seconds / 60);
  snprintf(value, sizeof(value), tr(STR_SLEEP_TIMER_VALUE_FORMAT), static_cast<unsigned>(minutes));
  return seconds > 0 && seconds < 60 ? "<" + std::string(value) : std::string(value);
}

std::string formatHomeReadingTime(const uint32_t seconds) {
  const std::string duration = formatDuration(seconds);
  char value[96]{};
  snprintf(value, sizeof(value), tr(STR_HOME_READING_TIME_FORMAT), duration.c_str());
  return value;
}

std::string formatTodayReadingSummary(const HomeBookSummary& summary) {
  if (!summary.hasTodayReadingSeconds) return tr(STR_HOME_TODAY_UNKNOWN);
  if (summary.todayReadingSeconds == 0) return tr(STR_HOME_NOT_READ);

  const std::string duration = formatDuration(summary.todayReadingSeconds);
  char value[96]{};
  if (summary.todayReadingSessions == 1) {
    snprintf(value, sizeof(value), tr(STR_HOME_ONE_SESSION_FORMAT), duration.c_str());
  } else {
    snprintf(value, sizeof(value), tr(STR_HOME_SESSIONS_FORMAT), duration.c_str(),
             static_cast<unsigned>(summary.todayReadingSessions));
  }
  return value;
}

StrId homeBookActionLabelId(const HomeBookSummary& summary) {
  switch (homeBookAction(summary)) {
    case HomeBookAction::Continue:
      return StrId::STR_CONTINUE_READING;
    case HomeBookAction::ReadAgain:
      return StrId::STR_READ_AGAIN;
    case HomeBookAction::Start:
      return StrId::STR_START_BOOK;
    case HomeBookAction::Open:
    default:
      return StrId::STR_OPEN_BOOK;
  }
}

std::string displayTitleForBook(const RecentBook& book) {
  std::string title = book.title.empty() ? book.path : book.title;
  const size_t lastSlash = title.find_last_of('/');
  if (lastSlash != std::string::npos) title.erase(0, lastSlash + 1);

  size_t extensionLength = 0;
  if (FsHelpers::hasEpubExtension(title) || FsHelpers::checkFileExtension(title, ".xtch")) {
    extensionLength = 5;
  } else if (FsHelpers::hasTxtExtension(title) || FsHelpers::checkFileExtension(title, ".xtc") ||
             FsHelpers::checkFileExtension(title, ".pdf")) {
    extensionLength = 4;
  } else if (FsHelpers::hasMarkdownExtension(title)) {
    extensionLength = 3;
  }
  if (extensionLength > 0) title.resize(title.size() - extensionLength);

  std::replace(title.begin(), title.end(), '_', ' ');
  title.erase(std::unique(title.begin(), title.end(), [](const char left, const char right) {
                return left == ' ' && right == ' ';
              }),
              title.end());
  return title;
}

void drawMenuIcon(const GfxRenderer& renderer, const uint8_t* bitmap, const int x, const int y, const int size,
                  const bool black) {
  if (black) {
    renderer.drawIcon(bitmap, x, y, size);
    return;
  }

  const int rowBytes = (size + 7) / 8;
  for (int row = 0; row < size; ++row) {
    for (int column = 0; column < size; ++column) {
      const uint8_t byte = bitmap[row * rowBytes + (column >> 3)];
      if (((byte >> (7 - (column & 7))) & 1) == 0) renderer.drawPixel(x + size - 1 - row, y + column, false);
    }
  }
}

void drawStatsIcon(const GfxRenderer& renderer, const int x, const int y, const bool black) {
  renderer.drawRoundedRect(x, y, MENU_ICON_SIZE, MENU_ICON_SIZE, 1, 3, true, true, true, true, black);
  renderer.fillRect(x + 6, y + 19, 4, 7, black);
  renderer.fillRect(x + 14, y + 13, 4, 13, black);
  renderer.fillRect(x + 22, y + 7, 4, 19, black);
}

void drawBookmarkIcon(const GfxRenderer& renderer, const int x, const int y, const bool black) {
  renderer.drawLine(x + 7, y + 5, x + 24, y + 5, black);
  renderer.drawLine(x + 7, y + 6, x + 24, y + 6, black);
  renderer.drawLine(x + 7, y + 5, x + 7, y + 27, black);
  renderer.drawLine(x + 8, y + 5, x + 8, y + 27, black);
  renderer.drawLine(x + 23, y + 5, x + 23, y + 27, black);
  renderer.drawLine(x + 24, y + 5, x + 24, y + 27, black);
  renderer.drawLine(x + 7, y + 27, x + 15, y + 21, black);
  renderer.drawLine(x + 8, y + 27, x + 15, y + 22, black);
  renderer.drawLine(x + 15, y + 21, x + 24, y + 27, black);
  renderer.drawLine(x + 15, y + 22, x + 23, y + 27, black);
}

bool finalLineIsEllipsized(const std::vector<std::string>& lines) {
  constexpr char ELLIPSIS[] = "\xE2\x80\xA6";
  return !lines.empty() && lines.back().size() >= sizeof(ELLIPSIS) - 1 &&
         lines.back().compare(lines.back().size() - (sizeof(ELLIPSIS) - 1), sizeof(ELLIPSIS) - 1, ELLIPSIS) == 0;
}

void drawCenteredText(const GfxRenderer& renderer, const int fontId, const Rect rect, const char* text,
                      const bool black = true, const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const int textWidth = renderer.getTextWidth(fontId, text, style);
  const int lineHeight = renderer.getLineHeight(fontId);
  renderer.drawText(fontId, rect.x + std::max(0, (rect.width - textWidth) / 2),
                    rect.y + std::max(0, (rect.height - lineHeight) / 2), text, black, style);
}

}  // namespace

void CrossViTheme::drawHomeHeader(const GfxRenderer& renderer, const Rect rect, const char* title) const {
  (void)title;
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const int batteryX =
      rect.x + rect.width - CrossViMetrics::values.contentSidePadding - CrossViMetrics::values.batteryWidth;
  drawBatteryRight(
      renderer, Rect{batteryX, rect.y + 8, CrossViMetrics::values.batteryWidth, CrossViMetrics::values.batteryHeight},
      showBatteryPercentage);

  const char* displayName = SETTINGS.deviceDisplayName[0] != '\0' ? SETTINGS.deviceDisplayName
                                                                  : (gpio.deviceIsX3() ? "Xteink X3" : "Xteink X4");
  const int nameMaxWidth = std::max(0, rect.width - CrossViMetrics::values.contentSidePadding * 2 - 94);
  const std::string safeName = renderer.truncatedText(UI_10_FONT_ID, displayName, nameMaxWidth, EpdFontFamily::REGULAR);
  renderer.drawText(UI_10_FONT_ID, rect.x + CrossViMetrics::values.contentSidePadding, rect.y + 9, safeName.c_str());
}

Rect CrossViTheme::getHomeCoverCacheRect(const Rect tileRect) const { return CrossViLayout::calculate(tileRect).cover; }

void CrossViTheme::drawHomeContent(GfxRenderer& renderer, const Rect rect, const std::vector<RecentBook>& recentBooks,
                                   const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                   bool& bufferRestored, std::function<bool()> storeCoverBuffer,
                                   const HomeBookSummary& summary) const {
  const CrossViLayout layout = CrossViLayout::calculate(rect);
  const bool hasBook = !recentBooks.empty();
  const bool selected = hasBook && selectorIndex == 0;

  renderer.drawRoundedRect(layout.card.x, layout.card.y, layout.card.width, layout.card.height, selected ? 2 : 1,
                           CORNER_RADIUS, true, true, true, true, true);

  if (!hasBook) {
    const int iconX = layout.card.x + 22;
    const int iconY = layout.card.y + (layout.card.height - 32) / 2;
    renderer.drawIcon(CoverIcon, iconX, iconY, 32);
    const int textX = iconX + 48;
    const int textWidth = std::max(0, layout.card.x + layout.card.width - textX - 20);
    const std::string titleText =
        renderer.truncatedText(UI_12_FONT_ID, tr(STR_NO_OPEN_BOOK), textWidth, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, textX, layout.card.y + layout.card.height / 2 - 28, titleText.c_str(), true,
                      EpdFontFamily::BOLD);
    const std::string help = renderer.truncatedText(UI_10_FONT_ID, tr(STR_START_READING), textWidth);
    renderer.drawText(UI_10_FONT_ID, textX, layout.card.y + layout.card.height / 2 + 8, help.c_str());
    return;
  }

  const RecentBook& book = recentBooks.front();
  if (!bufferRestored || !coverRendered) {
    bool drewCover = false;
    if (!book.coverBmpPath.empty()) {
      const std::string path = UITheme::getCoverThumbPath(book.coverBmpPath, CrossViMetrics::values.homeCoverHeight);
      HalFile file;
      if (Storage.openFileForRead("CROSSVI", path, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderer.drawBitmap(bitmap, layout.cover.x, layout.cover.y, layout.cover.width, layout.cover.height);
          drewCover = true;
        }
      }
    }
    if (!drewCover) {
      renderer.fillRoundedRect(layout.cover.x, layout.cover.y, layout.cover.width, layout.cover.height, 3,
                               Color::LightGray);
      const int coverLabelHeight = renderer.getLineHeight(SMALL_FONT_ID);
      const int placeholderHeight = 32 + 8 + coverLabelHeight;
      const int placeholderY = layout.cover.y + std::max(0, (layout.cover.height - placeholderHeight) / 2);
      renderer.drawIcon(CoverIcon, layout.cover.x + std::max(0, (layout.cover.width - 32) / 2), placeholderY, 32);
      const Rect coverLabel{layout.cover.x, placeholderY + 40, layout.cover.width, coverLabelHeight};
      drawCenteredText(renderer, SMALL_FONT_ID, coverLabel, tr(STR_COVER));
    }
    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }

  const std::string displayTitle = displayTitleForBook(book);
  int titleFontId = UI_12_FONT_ID;
  std::vector<std::string> titleLines =
      renderer.wrappedText(titleFontId, displayTitle.c_str(), layout.title.width, 2, EpdFontFamily::BOLD);
  if (finalLineIsEllipsized(titleLines)) {
    titleFontId = UI_10_FONT_ID;
    titleLines = renderer.wrappedText(titleFontId, displayTitle.c_str(), layout.title.width, 2, EpdFontFamily::BOLD);
  }
  int titleY = layout.title.y;
  const int titleLineHeight = renderer.getLineHeight(titleFontId);
  for (const std::string& line : titleLines) {
    renderer.drawText(titleFontId, layout.title.x, titleY, line.c_str(), true, EpdFontFamily::BOLD);
    titleY += titleLineHeight;
  }

  const int smallLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const HomeBookAction action = homeBookAction(summary);
  const bool hasTrustedReadingTime = summary.bookStatsState == DashboardMetricState::Available ||
                                     summary.bookStatsState == DashboardMetricState::NoData;
  if (action == HomeBookAction::Start || (summary.hasStartedReading && hasTrustedReadingTime)) {
    const std::string primary = action == HomeBookAction::Start
                                    ? std::string(tr(STR_HOME_NOT_STARTED))
                                    : (summary.bookReadingSeconds > 0 ? formatHomeReadingTime(summary.bookReadingSeconds)
                                                                    : std::string(tr(STR_HOME_IN_PROGRESS)));
    const std::string safePrimary = renderer.truncatedText(SMALL_FONT_ID, primary.c_str(), layout.stats.width);
    renderer.drawText(SMALL_FONT_ID, layout.stats.x,
                      layout.stats.y + std::max(0, (layout.stats.height - smallLineHeight) / 2), safePrimary.c_str());
  }

  const bool showProgress = hasReliableHomeBookProgress(summary) && summary.hasStartedReading;
  if (showProgress && layout.progressBar.width > 0 && layout.progressBar.height >= 10) {
    constexpr int BAR_HEIGHT = 10;
    const int barY = layout.progressBar.y + std::max(0, (layout.progressBar.height - BAR_HEIGHT) / 2);
    renderer.drawRoundedRect(layout.progressBar.x, barY, layout.progressBar.width, BAR_HEIGHT, 1, 3, true, true, true,
                             true, true);
    const int fillWidth = DashboardProgress::fillWidth(layout.progressBar.width, summary.progressPercent);
    if (fillWidth > 0) {
      renderer.fillRoundedRect(layout.progressBar.x + 2, barY + 2, fillWidth, BAR_HEIGHT - 4, 1, Color::Black);
    }
    char progressValue[12]{};
    if (summary.progressBelowOnePercent) {
      snprintf(progressValue, sizeof(progressValue), "<1%%");
    } else {
      snprintf(progressValue, sizeof(progressValue), "%u%%", static_cast<unsigned>(summary.progressPercent));
    }
    const int valueWidth = renderer.getTextWidth(SMALL_FONT_ID, progressValue);
    renderer.drawText(SMALL_FONT_ID, layout.progressLabel.x + std::max(0, layout.progressLabel.width - valueWidth),
                      layout.progressLabel.y + std::max(0, (layout.progressLabel.height - smallLineHeight) / 2),
                      progressValue);
  }

  Rect continueButton = showProgress ? layout.continueButton : layout.continueButtonWithoutProgress;
  if (continueButton.height > 0) {
    const char* actionText = I18N.get(homeBookActionLabelId(summary));
    const int actionWidth = renderer.getTextWidth(UI_10_FONT_ID, actionText, EpdFontFamily::BOLD);
    const int maxButtonWidth = std::max(0, layout.details.width - 16);
    continueButton.width = std::min(maxButtonWidth, std::max(208, actionWidth + 24));
    continueButton.x = layout.details.x + std::max(0, (layout.details.width - continueButton.width) / 2);
    if (selected) {
      renderer.fillRoundedRect(continueButton.x, continueButton.y, continueButton.width, continueButton.height,
                               CORNER_RADIUS, Color::Black);
    } else {
      renderer.drawRoundedRect(continueButton.x, continueButton.y, continueButton.width, continueButton.height, 1,
                               CORNER_RADIUS, true, true, true, true, true);
    }
    const std::string action = renderer.truncatedText(
        UI_10_FONT_ID, actionText, std::max(0, continueButton.width - 24), EpdFontFamily::BOLD);
    drawCenteredText(renderer, UI_10_FONT_ID, continueButton, action.c_str(), !selected, EpdFontFamily::BOLD);
  }
}

void CrossViTheme::drawButtonMenu(GfxRenderer& renderer, const Rect rect, const int buttonCount,
                                  const int selectedIndex, const std::function<std::string(int index)>& buttonLabel,
                                  const std::function<UIIcon(int index)>& rowIcon) const {
  const int tileWidth = (rect.width - MENU_SIDE_PADDING * 2 - MENU_GAP) / MENU_COLUMNS;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const HomeMenuLayout::Fit layout =
      HomeMenuLayout::fit(rect.height, buttonCount, MENU_COLUMNS, CrossViMetrics::values.menuRowHeight,
                          CrossViMetrics::values.menuSpacing, std::max(MENU_ICON_SIZE, lineHeight) + 12);
  const int menuOffset = std::min(24, std::max(0, (rect.height - layout.totalHeight()) / 2));

  for (int i = 0; i < buttonCount; ++i) {
    const int column = i % MENU_COLUMNS;
    const Rect tile{rect.x + MENU_SIDE_PADDING + column * (tileWidth + MENU_GAP),
                    rect.y + menuOffset + layout.yOffset(i, MENU_COLUMNS), tileWidth, layout.rowHeight};
    const bool selected = i == selectedIndex;

    if (selected) {
      renderer.fillRoundedRect(tile.x, tile.y, tile.width, tile.height, CORNER_RADIUS, Color::Black);
    } else {
      renderer.drawRoundedRect(tile.x, tile.y, tile.width, tile.height, 1, CORNER_RADIUS, true, true, true, true, true);
    }

    int contentX = tile.x + MENU_CONTENT_PADDING;
    if (rowIcon) {
      const UIIcon iconName = rowIcon(i);
      if (iconName == UIIcon::Book) {
        drawStatsIcon(renderer, contentX, tile.y + (tile.height - MENU_ICON_SIZE) / 2, !selected);
        contentX += MENU_ICON_SIZE + 10;
      } else if (iconName == UIIcon::Bookmark) {
        drawBookmarkIcon(renderer, contentX, tile.y + (tile.height - MENU_ICON_SIZE) / 2, !selected);
        contentX += MENU_ICON_SIZE + 10;
      } else if (const uint8_t* icon = iconForName(iconName)) {
        drawMenuIcon(renderer, icon, contentX, tile.y + (tile.height - MENU_ICON_SIZE) / 2, MENU_ICON_SIZE, !selected);
        contentX += MENU_ICON_SIZE + 10;
      }
    }

    const std::string rawLabel = buttonLabel(i);
    const EpdFontFamily::Style style = selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const std::string label = renderer.truncatedText(UI_10_FONT_ID, rawLabel.c_str(),
                                                     std::max(0, tile.x + tile.width - contentX - 10), style);
    renderer.drawText(UI_10_FONT_ID, contentX, tile.y + (tile.height - lineHeight) / 2, label.c_str(), !selected, style);
  }
}

void CrossViTheme::drawHomeReadingSummary(const GfxRenderer& renderer, const Rect rect,
                                          const HomeBookSummary& summary, const bool) const {
  if (rect.width <= 0 || rect.height <= 0) return;
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, CORNER_RADIUS, true, true, true, true, true);

  constexpr int HORIZONTAL_PADDING = 14;
  constexpr int LABEL_WIDTH = 88;
  constexpr int ROW_HEIGHT = 24;
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  constexpr bool textColor = true;
  const int labelX = rect.x + HORIZONTAL_PADDING;
  const int valueX = labelX + LABEL_WIDTH;
  const int valueRight = rect.x + rect.width - HORIZONTAL_PADDING;
  const int firstRowY = rect.y + 7;
  const int secondRowY = rect.y + rect.height - 7 - ROW_HEIGHT;

  renderer.drawText(SMALL_FONT_ID, labelX, firstRowY + std::max(0, (ROW_HEIGHT - lineHeight) / 2),
                    tr(STR_HOME_TODAY), textColor, EpdFontFamily::BOLD);
  const std::string today = formatTodayReadingSummary(summary);
  const std::string safeToday =
      renderer.truncatedText(SMALL_FONT_ID, today.c_str(), std::max(0, valueRight - valueX));
  renderer.drawText(SMALL_FONT_ID, valueX, firstRowY + std::max(0, (ROW_HEIGHT - lineHeight) / 2),
                    safeToday.c_str(), textColor);

  renderer.drawText(SMALL_FONT_ID, labelX, secondRowY + std::max(0, (ROW_HEIGHT - lineHeight) / 2),
                    tr(STR_HOME_GOAL), textColor, EpdFontFamily::BOLD);
  if (summary.dailyReadingGoalMinutes == 0) {
    const std::string unset = renderer.truncatedText(SMALL_FONT_ID, tr(STR_HOME_GOAL_UNSET),
                                                     std::max(0, valueRight - valueX));
    renderer.drawText(SMALL_FONT_ID, valueX, secondRowY + std::max(0, (ROW_HEIGHT - lineHeight) / 2), unset.c_str(),
                      textColor);
  } else {
    const uint32_t todayMinutes = summary.todayReadingSeconds == 0
                                      ? 0
                                      : std::max<uint32_t>(1, summary.todayReadingSeconds / 60u);
    char progressText[32]{};
    if (summary.hasTodayReadingSeconds) {
      snprintf(progressText, sizeof(progressText), tr(STR_HOME_GOAL_PROGRESS_FORMAT),
               static_cast<unsigned>(todayMinutes), static_cast<unsigned>(summary.dailyReadingGoalMinutes));
    } else {
      snprintf(progressText, sizeof(progressText), tr(STR_HOME_GOAL_PROGRESS_UNKNOWN_FORMAT),
               static_cast<unsigned>(summary.dailyReadingGoalMinutes));
    }
    const int progressTextWidth = renderer.getTextWidth(SMALL_FONT_ID, progressText);
    const int progressTextX = valueRight - progressTextWidth;
    constexpr int BAR_GAP = 10;
    constexpr int BAR_HEIGHT = 8;
    const int barWidth = std::max(0, progressTextX - BAR_GAP - valueX);
    const int barY = secondRowY + (ROW_HEIGHT - BAR_HEIGHT) / 2;
    if (barWidth >= 12) {
      renderer.drawRoundedRect(valueX, barY, barWidth, BAR_HEIGHT, 1, 2, true, true, true, true, textColor);
      const uint32_t cappedMinutes = summary.hasTodayReadingSeconds
                                         ? std::min<uint32_t>(todayMinutes, summary.dailyReadingGoalMinutes)
                                         : 0;
      const int fillWidth = static_cast<int>(static_cast<uint64_t>(std::max(0, barWidth - 4)) * cappedMinutes /
                                             summary.dailyReadingGoalMinutes);
      if (fillWidth > 0) renderer.fillRect(valueX + 2, barY + 2, fillWidth, BAR_HEIGHT - 4, textColor);
    }
    renderer.drawText(SMALL_FONT_ID, progressTextX,
                      secondRowY + std::max(0, (ROW_HEIGHT - lineHeight) / 2), progressText, textColor);
  }

}
