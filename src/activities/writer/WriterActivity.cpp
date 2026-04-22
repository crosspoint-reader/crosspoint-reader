#include "WriterActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <vector>

#include "WriterDraftStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

void WriterActivity::onEnter() {
  Activity::onEnter();

  inputBuffer = "Text to append\n";
  draftStore.ensureDraft();
  draftStore.readDraft(draftText);
  requestUpdate();
}

void WriterActivity::loop() {
  if (mappedInput.isPressed(MappedInputManager::Button::Back)) {
    finish();
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    draftStore.appendToDraft(inputBuffer);
    draftStore.readDraft(draftText);
    requestUpdate();
  }
}

void WriterActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto contentWidth = pageWidth - 2 * metrics.contentSidePadding;
  const auto x = metrics.contentSidePadding;
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  std::vector<std::string> visibleLines;  // Small screen buffer for the last 'x' lines

  // Read the file and keep the last 'x' lines
  constexpr int maxVisibleLines = 10;

  auto keepLastVisibleLines = [&] {
    while (visibleLines.size() > maxVisibleLines) {
      visibleLines.erase(visibleLines.begin());
    }
  };

  size_t start = 0;
  while (start <= draftText.size()) {
    size_t end = draftText.find('\n', start);
    std::string paragraph = draftText.substr(start, end == std::string::npos ? std::string::npos : end - start);

    if (paragraph.empty()) {
      visibleLines.push_back("");
      keepLastVisibleLines();
    } else {
      auto wrapped = renderer.wrappedText(UI_10_FONT_ID, paragraph.c_str(), contentWidth, maxVisibleLines);
      for (const auto& line : wrapped) {
        visibleLines.push_back(line);
        keepLastVisibleLines();
      }
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }

  // Draw the screen
  int y = metrics.topPadding + metrics.verticalSpacing;

  for (const auto& line : visibleLines) {
    renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
    y += lineHeight;
  }

  WriterActivity::renderFooter();

  renderer.displayBuffer();
}

int WriterActivity::countWords() const {
  int words = 0;
  bool inWord = false;

  for (const unsigned char ch : draftText) {
    if (std::isspace(ch)) {
      inWord = false;
    } else if (!inWord) {
      words++;
      inWord = true;
    }
  }

  return words;
}

// Standard footer isn't writer-ready, so we'll keep our version local.
void WriterActivity::renderFooter() const {
  const auto& metrics = UITheme::getInstance().getMetrics();

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  const auto screenHeight = renderer.getScreenHeight();
  const auto footerY = screenHeight - metrics.statusBarVerticalMargin - orientedMarginBottom - 4;

  // Battery display as per user settings
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;

  if (SETTINGS.statusBarBattery) {
    GUI.drawBatteryLeft(renderer,
                        Rect{metrics.statusBarHorizontalMargin + orientedMarginLeft + 1, footerY, metrics.batteryWidth,
                             metrics.batteryHeight},
                        showBatteryPercentage);
  }

  // Filename we're working on
  std::string title = draftStore.getDraftDisplayName();
  int titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
  renderer.drawText(SMALL_FONT_ID, (renderer.getScreenWidth() - titleWidth) / 2, footerY, title.c_str());

  // Current wordcount
  std::string wordCount = std::to_string(countWords()) + " words";
  int wordCountWidth = renderer.getTextWidth(SMALL_FONT_ID, wordCount.c_str());

  renderer.drawText(
      SMALL_FONT_ID,
      renderer.getScreenWidth() - metrics.statusBarHorizontalMargin - orientedMarginRight - wordCountWidth, footerY,
      wordCount.c_str());

  return;
}
