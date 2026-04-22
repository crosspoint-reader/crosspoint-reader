#include "WriterActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "WriterDraftStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

void WriterActivity::onEnter() {
  Activity::onEnter();

  WriterDraftStore draftStore;
  draftStore.ensureDraft();

  requestUpdate();
}

void WriterActivity::loop() {
  if (mappedInput.isPressed(MappedInputManager::Button::Back)) {
    finish();
  }
}

void WriterActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto x = metrics.contentSidePadding;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_WRITER));
  renderer.drawText(UI_10_FONT_ID, x, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing,
                    "Writer Mode PlaceHolder");

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
