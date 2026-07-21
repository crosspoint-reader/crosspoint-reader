#include "ReadingStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <numeric>
#include <string>
#include <utility>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
enum class MetricFormat : uint8_t { Duration, Count, Pace, Percent, Completion, Remaining, Date };

struct SummaryCell {
  StrId label;
  const ReadingStatsMetric* metric;
  MetricFormat format;
};

constexpr std::array<StrId, READING_TIME_BUCKET_COUNT> TIME_BUCKET_LABELS = {
    StrId::STR_STATS_MORNING, StrId::STR_STATS_AFTERNOON, StrId::STR_STATS_EVENING, StrId::STR_STATS_NIGHT};
constexpr std::array<StrId, READING_DAY_OF_WEEK_COUNT> DAY_LABELS = {
    StrId::STR_STATS_MON, StrId::STR_STATS_TUE, StrId::STR_STATS_WED, StrId::STR_STATS_THU,
    StrId::STR_STATS_FRI, StrId::STR_STATS_SAT, StrId::STR_STATS_SUN};

std::string formatDuration(const uint32_t seconds, const bool estimated, const bool preferSeconds = false) {
  char value[28];
  const char* prefix = estimated ? "~" : "";
  if (seconds == 0) {
    snprintf(value, sizeof(value), "%s0m", prefix);
  } else if (preferSeconds && seconds < 60) {
    snprintf(value, sizeof(value), "%s%lus", prefix, static_cast<unsigned long>(seconds));
  } else if (seconds < 60) {
    snprintf(value, sizeof(value), "%s<1m", prefix);
  } else {
    const uint32_t minutes = seconds / 60;
    if (minutes < 60) {
      snprintf(value, sizeof(value), "%s%lum", prefix, static_cast<unsigned long>(minutes));
    } else {
      const uint32_t hours = minutes / 60;
      const uint32_t remainder = minutes % 60;
      if (hours < 1000 && remainder > 0) {
        snprintf(value, sizeof(value), "%s%luh %lum", prefix, static_cast<unsigned long>(hours),
                 static_cast<unsigned long>(remainder));
      } else {
        snprintf(value, sizeof(value), "%s%luh", prefix, static_cast<unsigned long>(hours));
      }
    }
  }
  return value;
}

std::string formatMetric(const ReadingStatsMetric& metric, const MetricFormat format) {
  if (metric.state == ReadingStatsMetricState::Unavailable) return tr(STR_STATS_UNAVAILABLE);
  const bool estimated = metric.state == ReadingStatsMetricState::Estimated;
  switch (format) {
    case MetricFormat::Duration:
      return formatDuration(metric.value, estimated);
    case MetricFormat::Pace:
      return formatDuration(metric.value, estimated, true);
    case MetricFormat::Percent:
      return std::string(estimated ? "~" : "") + std::to_string(metric.value) + "%";
    case MetricFormat::Completion:
      return I18N.get(metric.value != 0 ? StrId::STR_YES : StrId::STR_NO);
    case MetricFormat::Remaining:
      return metric.state == ReadingStatsMetricState::Known && metric.value == 0
                 ? std::string(tr(STR_STATS_FINISHED))
                 : formatDuration(metric.value, estimated);
    case MetricFormat::Date: {
      ReadingStatsDate date;
      if (!readingStatsDateFromDayIndex(metric.value, date)) return tr(STR_STATS_UNAVAILABLE);
      char value[20];
      snprintf(value, sizeof(value), "%s%04u-%02u-%02u", estimated ? "~" : "", static_cast<unsigned>(date.year),
               static_cast<unsigned>(date.month), static_cast<unsigned>(date.day));
      return value;
    }
    case MetricFormat::Count:
    default:
      return std::string(estimated ? "~" : "") + std::to_string(metric.value);
  }
}

void drawCentered(const GfxRenderer& renderer, const int fontId, const int x, const int width, const int y,
                  const char* value, const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const int textWidth = renderer.getTextWidth(fontId, value, style);
  renderer.drawText(fontId, x + std::max(0, (width - textWidth) / 2), y, value, true, style);
}

void drawSummaryCell(const GfxRenderer& renderer, const Rect& rect, const SummaryCell& cell) {
  const int valueLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int labelLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int textHeight = valueLineHeight + labelLineHeight + 2;
  const int top = rect.y + std::max(2, (rect.height - textHeight) / 2);
  const std::string value = formatMetric(*cell.metric, cell.format);
  const std::string label = renderer.truncatedText(SMALL_FONT_ID, I18N.get(cell.label), rect.width - 8);
  drawCentered(renderer, UI_10_FONT_ID, rect.x + 4, rect.width - 8, top, value.c_str(), EpdFontFamily::BOLD);
  drawCentered(renderer, SMALL_FONT_ID, rect.x + 4, rect.width - 8, top + valueLineHeight + 2, label.c_str());
}

template <size_t N>
void drawSummaryCard(const GfxRenderer& renderer, const Rect& rect, const std::array<SummaryCell, N>& cells,
                     const int columns) {
  const int rows = (static_cast<int>(N) + columns - 1) / columns;
  const int columnWidth = rect.width / columns;
  const int rowHeight = rect.height / rows;
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
  for (int column = 1; column < columns; ++column) {
    renderer.drawLine(rect.x + column * columnWidth, rect.y, rect.x + column * columnWidth, rect.y + rect.height - 1);
  }
  for (int row = 1; row < rows; ++row) {
    renderer.drawLine(rect.x, rect.y + row * rowHeight, rect.x + rect.width - 1, rect.y + row * rowHeight);
  }
  for (size_t index = 0; index < N; ++index) {
    const int row = static_cast<int>(index) / columns;
    const int column = static_cast<int>(index) % columns;
    const int cellX = rect.x + column * columnWidth;
    const int cellWidth = column == columns - 1 ? rect.x + rect.width - cellX : columnWidth;
    const int cellY = rect.y + row * rowHeight;
    const int cellHeight = row == rows - 1 ? rect.y + rect.height - cellY : rowHeight;
    drawSummaryCell(renderer, Rect{cellX, cellY, cellWidth, cellHeight}, cells[index]);
  }
}

std::string formatChartDuration(const uint32_t seconds) {
  char value[20];
  if (seconds == 0) {
    snprintf(value, sizeof(value), "0m");
  } else if (seconds < 60) {
    snprintf(value, sizeof(value), "<1m");
  } else if (seconds < 3600) {
    snprintf(value, sizeof(value), "%lum", static_cast<unsigned long>(seconds / 60));
  } else if (seconds < 100u * 3600u) {
    snprintf(value, sizeof(value), "%luh %lum", static_cast<unsigned long>(seconds / 3600),
             static_cast<unsigned long>(seconds % 3600 / 60));
  } else {
    snprintf(value, sizeof(value), "%luh", static_cast<unsigned long>(seconds / 3600));
  }
  return value;
}

template <size_t N>
void drawChart(GfxRenderer& renderer, const Rect& rect, const StrId title, const ReadingStatsChart<N>& chart,
               const std::array<StrId, N>& labels) {
  if (rect.width <= 2 || rect.height <= 2) return;
  const int titleHeight = renderer.getLineHeight(UI_10_FONT_ID) + 8;
  const int noteHeight = chart.incomplete ? renderer.getLineHeight(SMALL_FONT_ID) + 5 : 0;
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
  renderer.drawLine(rect.x, rect.y + titleHeight, rect.x + rect.width - 1, rect.y + titleHeight);
  drawCentered(renderer, UI_10_FONT_ID, rect.x + 4, rect.width - 8, rect.y + 4, I18N.get(title), EpdFontFamily::BOLD);

  if (!chart.available) {
    const int messageY =
        rect.y + titleHeight + std::max(0, (rect.height - titleHeight - renderer.getLineHeight(SMALL_FONT_ID)) / 2);
    const std::string message =
        renderer.truncatedText(SMALL_FONT_ID, tr(STR_STATS_DATED_DATA_UNAVAILABLE), rect.width - 16);
    drawCentered(renderer, SMALL_FONT_ID, rect.x + 8, rect.width - 16, messageY, message.c_str());
    return;
  }

  const int bodyTop = rect.y + titleHeight;
  const int bodyHeight = std::max(1, rect.height - titleHeight - noteHeight);
  const int rowHeight = std::max(1, bodyHeight / static_cast<int>(N));
  int labelWidth = std::accumulate(labels.begin(), labels.end(), 0, [&renderer](const int width, const StrId label) {
    return std::max(width, renderer.getTextWidth(SMALL_FONT_ID, I18N.get(label)));
  });
  labelWidth = std::min(labelWidth + 12, rect.width / 3);
  const int valueWidth = std::max(42, renderer.getTextWidth(SMALL_FONT_ID, "99999h") + 8);
  const int barX = rect.x + 6 + labelWidth;
  const int barWidth = std::max(0, rect.width - labelWidth - valueWidth - 18);
  const uint32_t maximum = *std::max_element(chart.seconds.begin(), chart.seconds.end());
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);

  for (size_t index = 0; index < N; ++index) {
    const int rowTop = bodyTop + static_cast<int>(index) * rowHeight;
    const int textY = rowTop + std::max(0, (rowHeight - lineHeight) / 2);
    const std::string label = renderer.truncatedText(SMALL_FONT_ID, I18N.get(labels[index]), labelWidth - 4);
    renderer.drawText(SMALL_FONT_ID, rect.x + 6, textY, label.c_str());

    if (barWidth > 2 && rowHeight > 4) {
      const int barHeight = std::max(3, std::min(12, rowHeight - 6));
      const int barY = rowTop + std::max(1, (rowHeight - barHeight) / 2);
      renderer.drawRect(barX, barY, barWidth, barHeight);
      const int fillWidth = scaleReadingStatsBar(chart.seconds[index], maximum, barWidth - 2);
      if (fillWidth > 0) renderer.fillRect(barX + 1, barY + 1, fillWidth, barHeight - 2);
    }

    const std::string value = formatChartDuration(chart.seconds[index]);
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, value.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - 6 - textWidth, textY, value.c_str());
  }

  if (chart.incomplete) {
    const int noteY = rect.y + rect.height - noteHeight + 2;
    const std::string note = renderer.truncatedText(SMALL_FONT_ID, tr(STR_STATS_DATED_DATA_PARTIAL), rect.width - 12);
    drawCentered(renderer, SMALL_FONT_ID, rect.x + 6, rect.width - 12, noteY, note.c_str());
  }
}

std::array<SummaryCell, 9> bookCells(const BookReadingStatsPresentation& model) {
  const StrId finishLabel = model.finishDate.state == ReadingStatsMetricState::Estimated
                                ? StrId::STR_STATS_EST_FINISH_DATE
                                : StrId::STR_STATS_FINISHED_DATE;
  return {{{StrId::STR_STATS_READING_TIME, &model.readingTime, MetricFormat::Duration},
           {StrId::STR_STATS_BOOK_SESSIONS, &model.sessions, MetricFormat::Count},
           {StrId::STR_STATS_PAGES_TURNED, &model.pagesTurned, MetricFormat::Count},
           {StrId::STR_STATS_PROGRESS, &model.progress, MetricFormat::Percent},
           {StrId::STR_STATS_AVG_PAGE, &model.averagePage, MetricFormat::Pace},
           {StrId::STR_STATS_TIME_LEFT, &model.timeLeft, MetricFormat::Remaining},
           {StrId::STR_STATS_COMPLETED, &model.completed, MetricFormat::Completion},
           {StrId::STR_STATS_STARTED_DATE, &model.startDate, MetricFormat::Date},
           {finishLabel, &model.finishDate, MetricFormat::Date}}};
}

std::array<SummaryCell, 7> globalCells(const GlobalReadingStatsPresentation& model) {
  return {{{StrId::STR_STATS_READING_TIME, &model.readingTime, MetricFormat::Duration},
           {StrId::STR_STATS_SESSIONS, &model.sessions, MetricFormat::Count},
           {StrId::STR_STATS_PAGES_TURNED, &model.pagesTurned, MetricFormat::Count},
           {StrId::STR_STATS_COMPLETED_BOOKS, &model.completedBooks, MetricFormat::Count},
           {StrId::STR_STATS_STREAK, &model.currentStreak, MetricFormat::Count},
           {StrId::STR_STATS_LONGEST_STREAK, &model.longestStreak, MetricFormat::Count},
           {StrId::STR_STATS_READING_DAYS, &model.readingDays, MetricFormat::Count}}};
}
}  // namespace

ReadingStatsActivity::ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           std::string bookTitle, ReadingStatsPresentation presentation)
    : Activity("ReadingStats", renderer, mappedInput),
      bookTitle(std::move(bookTitle)),
      presentation(std::move(presentation)) {}

void ReadingStatsActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void ReadingStatsActivity::loop() {
  buttonNavigator.onNext([this] {
    const int next = (static_cast<int>(page) + 1) % pageCount();
    page = static_cast<Page>(next);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    const int previous = (static_cast<int>(page) + pageCount() - 1) % pageCount();
    page = static_cast<Page>(previous);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
  }
}

void ReadingStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int headerTop = safeArea.y + metrics.topPadding;
  const int subHeaderTop = headerTop + metrics.headerHeight;
  const int contentTop = subHeaderTop + metrics.tabBarHeight + metrics.verticalSpacing;
  // getScreenSafeArea(..., true, ...) already removes the physical front-button
  // strip in every orientation. Keep an additional content gap inside that
  // safe area so charts can never be painted under the button hints.
  const int contentBottom = safeArea.y + safeArea.height - metrics.verticalSpacing;
  const int cardX = safeArea.x + metrics.contentSidePadding;
  const int cardWidth = std::max(1, safeArea.width - metrics.contentSidePadding * 2);
  const bool landscape = safeArea.width > safeArea.height;

  const GlobalReadingStatsPresentation* globalModel = nullptr;
  StrId pageTitle = StrId::STR_STATS_THIS_BOOK;
  std::string pageTitleText;
  int summaryRows = landscape ? 2 : 5;
  int summaryColumns = landscape ? 5 : 2;
  if (page == Page::Device) {
    pageTitle = StrId::STR_STATS_THIS_DEVICE;
    globalModel = &presentation.device;
    summaryRows = landscape ? 2 : 4;
    summaryColumns = landscape ? 4 : 2;
  } else if (page == Page::AllSynced) {
    pageTitle = StrId::STR_STATS_ALL_SYNCED;
    globalModel = &presentation.allSynced;
    summaryRows = landscape ? 2 : 4;
    summaryColumns = landscape ? 4 : 2;
  }

  if (page == Page::AllSynced) {
    char syncedTitle[64];
    snprintf(syncedTitle, sizeof(syncedTitle), tr(STR_STATS_ALL_SYNCED_FORMAT),
             static_cast<unsigned>(presentation.validPeerCount) + 1U);
    pageTitleText = syncedTitle;
  } else {
    pageTitleText = I18N.get(pageTitle);
  }

  char pageIndicator[16];
  snprintf(pageIndicator, sizeof(pageIndicator), "%u/%u", static_cast<unsigned>(page) + 1,
           static_cast<unsigned>(pageCount()));
  GUI.drawHeader(renderer, Rect{safeArea.x, headerTop, safeArea.width, metrics.headerHeight}, tr(STR_READING_STATS),
                 page == Page::Book ? bookTitle.c_str() : nullptr);
  GUI.drawSubHeader(renderer, Rect{safeArea.x, subHeaderTop, safeArea.width, metrics.tabBarHeight},
                    pageTitleText.c_str(), pageIndicator);

  const int summaryRowHeight = renderer.getLineHeight(UI_10_FONT_ID) + renderer.getLineHeight(SMALL_FONT_ID) + 8;
  const int summaryHeight = summaryRows * summaryRowHeight;
  const Rect summaryRect{cardX, contentTop, cardWidth, std::max(1, summaryHeight)};
  if (page == Page::Book) {
    drawSummaryCard(renderer, summaryRect, bookCells(presentation.book), summaryColumns);
  } else {
    drawSummaryCard(renderer, summaryRect, globalCells(*globalModel), summaryColumns);
  }

  int chartsTop = contentTop + summaryHeight + metrics.verticalSpacing;
  if (page == Page::AllSynced && !presentation.syncAggregateComplete) {
    const int warningHeight = renderer.getLineHeight(SMALL_FONT_ID) + 4;
    const std::string warning = renderer.truncatedText(SMALL_FONT_ID, tr(STR_STATS_SYNC_PARTIAL), cardWidth - 8);
    drawCentered(renderer, SMALL_FONT_ID, cardX + 4, cardWidth - 8, chartsTop, warning.c_str());
    chartsTop += warningHeight;
  }

  const int chartHeight = std::max(1, contentBottom - chartsTop);
  const auto& timeChart = page == Page::Book ? presentation.book.timeOfDay : globalModel->timeOfDay;
  const auto& dayChart = page == Page::Book ? presentation.book.dayOfWeek : globalModel->dayOfWeek;
  if (landscape) {
    const int gap = metrics.verticalSpacing;
    const int firstWidth = std::max(1, (cardWidth - gap) / 2);
    drawChart(renderer, Rect{cardX, chartsTop, firstWidth, chartHeight}, StrId::STR_STATS_TIME_OF_DAY, timeChart,
              TIME_BUCKET_LABELS);
    drawChart(renderer,
              Rect{cardX + firstWidth + gap, chartsTop, std::max(1, cardWidth - firstWidth - gap), chartHeight},
              StrId::STR_STATS_DAY_OF_WEEK, dayChart, DAY_LABELS);
  } else {
    const int gap = metrics.verticalSpacing;
    const int firstHeight = std::max(1, (chartHeight - gap) * 5 / 12);
    drawChart(renderer, Rect{cardX, chartsTop, cardWidth, firstHeight}, StrId::STR_STATS_TIME_OF_DAY, timeChart,
              TIME_BUCKET_LABELS);
    drawChart(renderer,
              Rect{cardX, chartsTop + firstHeight + gap, cardWidth, std::max(1, chartHeight - firstHeight - gap)},
              StrId::STR_STATS_DAY_OF_WEEK, dayChart, DAY_LABELS);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), tr(STR_STATS_PREVIOUS), tr(STR_STATS_NEXT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
