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
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_WRITER));

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  for (const auto& line : visibleLines) {
    renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
    y += lineHeight;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "Append", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
