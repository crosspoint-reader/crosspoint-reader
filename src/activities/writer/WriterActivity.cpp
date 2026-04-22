#include "WriterActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cctype>
#include <vector>

#include "Logging.h"
#include "WriterDraftStore.h"
#include "WriterInput.h"
#include "components/UITheme.h"
#include "fontIds.h"

void WriterActivity::onEnter() {
  Activity::onEnter();

  inputBuffer.clear();
  draftStore.ensureDraft();
  draftStore.readDraft(draftText);
  requestUpdate();
}

void WriterActivity::loop() {
  std::string inputText;
  if (WriterInput::readAvailable(inputText)) {
    inputBuffer += inputText;
    requestUpdate();
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Back)) {
    finish();
  }
  if (!inputBuffer.empty() && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (draftStore.appendToDraft(inputBuffer)) {
      inputBuffer.clear();
      draftStore.readDraft(draftText);
      requestUpdate();
    } else {
      LOG_ERR("Writer", "Failed to write to draft file: %s", WriterDraftStore::DraftPath);
    }
  }
}

void WriterActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto contentWidth = pageWidth - 2 * metrics.contentSidePadding;
  const auto x = metrics.contentSidePadding;
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int textTop = metrics.topPadding;
  const auto footer = getFooterLayout();

  const int availableTextHeight = footer.top - textTop - metrics.verticalSpacing;
  const int maxVisibleLines = std::max(1, availableTextHeight / lineHeight);

  const std::string renderedText = getRenderedText();
  std::vector<std::string> visibleLines;  // Small screen buffer for the last 'x' lines

  // Read the file and keep the last 'x' lines
  auto keepLastVisibleLines = [&] {
    while (visibleLines.size() > maxVisibleLines) {
      visibleLines.erase(visibleLines.begin());
    }
  };

  size_t start = 0;
  while (start <= renderedText.size()) {
    size_t end = renderedText.find('\n', start);
    std::string paragraph = renderedText.substr(start, end == std::string::npos ? std::string::npos : end - start);

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

std::string WriterActivity::getRenderedText() const { return draftText + inputBuffer; }

int WriterActivity::countWords(const std::string& text) const {
  int words = 0;
  bool inWord = false;

  for (const unsigned char ch : text) {
    if (std::isspace(ch)) {
      inWord = false;
    } else if (!inWord) {
      words++;
      inWord = true;
    }
  }

  return words;
}

WriterActivity::FooterLayout WriterActivity::getFooterLayout() const {
  const auto& metrics = UITheme::getInstance().getMetrics();

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  return FooterLayout{renderer.getScreenHeight() - metrics.statusBarVerticalMargin - orientedMarginBottom - 4,
                      orientedMarginLeft, orientedMarginRight};
}

// Standard footer isn't writer-ready, so we'll keep our version local.
void WriterActivity::renderFooter() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto footer = getFooterLayout();

  // Battery display as per user settings
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;

  if (SETTINGS.statusBarBattery) {
    GUI.drawBatteryLeft(renderer,
                        Rect{metrics.statusBarHorizontalMargin + footer.marginLeft + 1, footer.top,
                             metrics.batteryWidth, metrics.batteryHeight},
                        showBatteryPercentage);
  }

  // Filename we're working on
  std::string title = draftStore.getDraftDisplayName();
  int titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
  renderer.drawText(SMALL_FONT_ID, (renderer.getScreenWidth() - titleWidth) / 2, footer.top, title.c_str());

  // Current wordcount
  std::string wordCount = std::to_string(countWords(getRenderedText())) + " words";
  int wordCountWidth = renderer.getTextWidth(SMALL_FONT_ID, wordCount.c_str());

  renderer.drawText(SMALL_FONT_ID,
                    renderer.getScreenWidth() - metrics.statusBarHorizontalMargin - footer.marginRight - wordCountWidth,
                    footer.top, wordCount.c_str());

  return;
}
