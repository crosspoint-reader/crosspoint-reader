#include "ReaderStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

std::string formatTime(uint32_t seconds) {
  if (seconds < 60) {
    return "< 1 min";
  }
  const uint32_t minutes = seconds / 60;
  if (minutes < 60) {
    return std::to_string(minutes) + " min";
  }
  const uint32_t hours = minutes / 60;
  const uint32_t remainingMinutes = minutes % 60;
  return std::to_string(hours) + " h " + std::to_string(remainingMinutes) + " min";
}

}  // namespace

void ReaderStatsActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void ReaderStatsActivity::onExit() { Activity::onExit(); }

void ReaderStatsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  }
}

void ReaderStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_STATISTICS));

  const int marginLeft = metrics.contentSidePadding;
  const int sectionGap = metrics.verticalSpacing;
  constexpr int rowHeight = 26;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  if (showSession) {
    // --- This session ---
    renderer.drawText(UI_10_FONT_ID, marginLeft, y, tr(STR_STATS_SESSION), true, EpdFontFamily::BOLD);
    y += rowHeight;

    renderer.drawText(UI_10_FONT_ID, marginLeft, y,
                      (std::string(tr(STR_STATS_TIME)) + formatTime(sessionSeconds)).c_str());
    y += rowHeight;
    renderer.drawText(UI_10_FONT_ID, marginLeft, y,
                      (std::string(tr(STR_STATS_PAGES)) + std::to_string(sessionPages)).c_str());
    y += rowHeight;
    renderer.drawText(UI_10_FONT_ID, marginLeft, y,
                      (std::string(tr(STR_STATS_WORDS)) + std::to_string(sessionWords)).c_str());
    y += rowHeight;

    if (sessionSeconds >= 30 && sessionWords > 0) {
      const uint32_t wpm = (sessionWords * 60) / sessionSeconds;
      renderer.drawText(UI_10_FONT_ID, marginLeft, y,
                        (std::string(tr(STR_STATS_SPEED)) + std::to_string(wpm) + " wpm").c_str());
      y += rowHeight;
    }

    y += sectionGap;
  }

  // --- Current book ---
  renderer.drawText(UI_10_FONT_ID, marginLeft, y, tr(STR_STATS_CURRENT_BOOK), true, EpdFontFamily::BOLD);
  y += rowHeight;

  renderer.drawText(UI_10_FONT_ID, marginLeft, y,
                    (std::string(tr(STR_STATS_TIME)) + formatTime(bookStats.totalReadingSeconds)).c_str());
  y += rowHeight;
  renderer.drawText(UI_10_FONT_ID, marginLeft, y,
                    (std::string(tr(STR_STATS_PAGES)) + std::to_string(bookStats.totalPagesRead)).c_str());
  y += rowHeight;
  renderer.drawText(UI_10_FONT_ID, marginLeft, y,
                    (std::string(tr(STR_STATS_WORDS)) + std::to_string(bookStats.totalWordsRead)).c_str());
  y += rowHeight;
  renderer.drawText(UI_10_FONT_ID, marginLeft, y,
                    (std::string(tr(STR_STATS_SESSIONS)) + std::to_string(bookStats.sessionsCount)).c_str());
  y += rowHeight;
  renderer.drawText(
      UI_10_FONT_ID, marginLeft, y,
      (std::string(tr(STR_STATS_FINISHED)) + (bookStats.finished ? std::string(tr(STR_YES)) : std::string(tr(STR_NO))))
          .c_str());
  y += rowHeight + sectionGap;

  // --- All books ---
  renderer.drawText(UI_10_FONT_ID, marginLeft, y, tr(STR_STATS_ALL_BOOKS), true, EpdFontFamily::BOLD);
  y += rowHeight;

  renderer.drawText(UI_10_FONT_ID, marginLeft, y,
                    (std::string(tr(STR_STATS_TIME)) + formatTime(globalStats.totalReadingSeconds)).c_str());
  y += rowHeight;
  renderer.drawText(UI_10_FONT_ID, marginLeft, y,
                    (std::string(tr(STR_STATS_PAGES)) + std::to_string(globalStats.totalPagesRead)).c_str());
  y += rowHeight;
  renderer.drawText(UI_10_FONT_ID, marginLeft, y,
                    (std::string(tr(STR_STATS_WORDS)) + std::to_string(globalStats.totalWordsRead)).c_str());
  y += rowHeight;
  renderer.drawText(UI_10_FONT_ID, marginLeft, y,
                    (std::string(tr(STR_STATS_STARTED)) + std::to_string(globalStats.booksStarted)).c_str());
  y += rowHeight;
  renderer.drawText(UI_10_FONT_ID, marginLeft, y,
                    (std::string(tr(STR_STATS_FINISHED)) + std::to_string(globalStats.booksFinished)).c_str());

  // Footer
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
