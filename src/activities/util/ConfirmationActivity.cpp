#include "ConfirmationActivity.h"

#include <I18n.h>

#include "HalDisplay.h"
#include "components/UITheme.h"

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body)
    : Activity("Confirmation", renderer, mappedInput), heading(heading), body(body) {}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();

  lineHeight = renderer.getLineHeight(fontId);
  const int maxWidth = renderer.getScreenWidth() - (margin * 2);

  if (!heading.empty()) {
    safeHeading = renderer.truncatedText(fontId, heading.c_str(), maxWidth, EpdFontFamily::BOLD);
  }
  // The body wraps rather than truncating: a prompt explaining what is about to
  // happen is routinely longer than one line, and truncating it cut off the part
  // that mattered. The heading stays single-line — it's a title, not prose.
  //
  // A newline in the body starts a new paragraph, so callers can separate the
  // question being asked from the explanation of what it means instead of
  // running them together as one block.
  if (!body.empty()) {
    size_t start = 0;
    while (start <= body.size() && static_cast<int>(bodyLines.size()) < maxBodyLines) {
      const size_t newline = body.find('\n', start);
      const std::string paragraph = body.substr(start, newline == std::string::npos ? std::string::npos : newline - start);
      if (paragraph.empty()) {
        bodyLines.emplace_back();  // blank line between paragraphs
      } else {
        const int remaining = maxBodyLines - static_cast<int>(bodyLines.size());
        const auto wrapped = renderer.wrappedText(fontId, paragraph.c_str(), maxWidth, remaining, EpdFontFamily::REGULAR);
        bodyLines.insert(bodyLines.end(), wrapped.begin(), wrapped.end());
      }
      if (newline == std::string::npos) break;
      start = newline + 1;
    }
  }

  // Text sits in the upper part of the screen so the confirmation popup
  // (centered) doesn't cover it.
  startY = renderer.getScreenHeight() / 6;

  const char* options[] = {I18N.get(StrId::STR_CANCEL), I18N.get(StrId::STR_CONFIRM)};
  confirmPopup.show(safeHeading.c_str(), options, 2, 0, [this](int idx) {
    ActivityResult res;
    res.isCancelled = (idx != 1);
    setResult(std::move(res));
    finish();
  });

  requestUpdate(true);
}

void ConfirmationActivity::render(RenderLock&& lock) {
  renderer.clearScreen();

  int currentY = startY;
  LOG_DBG("CONF", "currentY: %d", currentY);
  // Draw Heading
  if (!safeHeading.empty()) {
    renderer.drawCenteredText(fontId, currentY, safeHeading.c_str(), true, EpdFontFamily::BOLD);
    currentY += lineHeight + spacing;
  }

  // Draw Body
  for (const auto& line : bodyLines) {
    renderer.drawCenteredText(fontId, currentY, line.c_str(), true, EpdFontFamily::REGULAR);
    currentY += lineHeight;
  }

  if (confirmPopup.processRender(renderer, mappedInput)) return;

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void ConfirmationActivity::loop() {
  if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  // Popup dismissed without a selection (Back button or tap outside): cancel.
  ActivityResult res;
  res.isCancelled = true;
  setResult(std::move(res));
  finish();
}
