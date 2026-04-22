#include "WriterActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "WriterDraftStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

void WriterActivity::onEnter() {
  Activity::onEnter();

  draftStore.ensureDraft();
  draftStore.readDraft(draftText);
  requestUpdate();
}

void WriterActivity::loop() {
  if (mappedInput.isPressed(MappedInputManager::Button::Back)) {
    finish();
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    draftStore.appendToDraft("\nAppended text.");
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

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_WRITER));

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  constexpr int maxVisibleLines = 10;
  int renderedLines = 0;

  size_t start = 0;
  while (start <= draftText.size() && renderedLines < maxVisibleLines) {
    size_t end = draftText.find('\n', start);
    std::string paragraph = draftText.substr(start, end == std::string::npos ? std::string::npos : end - start);

    if (paragraph.empty()) {
      y += lineHeight;
      renderedLines++;
    } else {
      auto wrapped =
          renderer.wrappedText(UI_10_FONT_ID, paragraph.c_str(), contentWidth, maxVisibleLines - renderedLines);
      for (const auto& line : wrapped) {
        renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
        y += lineHeight;
        renderedLines++;
      }
    }

    if (end == std::string::npos) break;
    start = end + 1;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "Append", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
