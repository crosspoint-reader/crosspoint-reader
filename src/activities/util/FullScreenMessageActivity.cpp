#include "FullScreenMessageActivity.h"

#include <GfxRenderer.h>

#include <vector>

#include "fontIds.h"

namespace {
// Greedy word-wrap: the existing single-line drawCenteredText() callers (e.g.
// the SD-card-error screen) all pass short text that never needed this, but
// a longer message (e.g. a settings-validation notice) must not run off
// either edge of the screen. Splits on plain ASCII spaces only (no
// <sstream>) -- every current caller's text is English, so this is
// deliberately not UTF-8/CJK-word-boundary aware. A caller passing a
// space-delimiter-free string (e.g. a future Han/Kana translation of one of
// these messages) would come back as a single unwrapped, overflowing line;
// revisit with that real string in hand rather than guessing at Unicode
// line-breaking rules ahead of need.
std::vector<std::string> wrapText(const GfxRenderer& renderer, const std::string& text, const int fontId,
                                  const EpdFontFamily::Style style, const int maxWidth) {
  std::vector<std::string> lines;
  // Cheap upper-bound estimate (unwrapped width / column width) so the
  // common case never grows the vector mid-loop.
  lines.reserve(static_cast<size_t>(renderer.getTextWidth(fontId, text.c_str(), style) / maxWidth) + 1);
  std::string line;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find(' ', start);
    if (end == std::string::npos) end = text.size();
    const std::string word = text.substr(start, end - start);
    const std::string candidate = line.empty() ? word : line + " " + word;
    if (!line.empty() && renderer.getTextWidth(fontId, candidate.c_str(), style) > maxWidth) {
      lines.push_back(line);
      line = word;
    } else {
      line = candidate;
    }
    start = end + 1;
  }
  if (!line.empty()) lines.push_back(line);
  return lines;
}
}  // namespace

void FullScreenMessageActivity::onEnter() {
  Activity::onEnter();

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int maxWidth = renderer.getScreenWidth() * 4 / 5;
  const std::vector<std::string> lines = wrapText(renderer, text, UI_10_FONT_ID, style, maxWidth);
  const auto top = (renderer.getScreenHeight() - static_cast<int>(lines.size()) * lineHeight) / 2;

  renderer.clearScreen();
  for (size_t i = 0; i < lines.size(); i++) {
    renderer.drawCenteredText(UI_10_FONT_ID, top + static_cast<int>(i) * lineHeight, lines[i].c_str(), true, style);
  }
  renderer.displayBuffer(refreshMode);
}

void FullScreenMessageActivity::loop() {
  if (!dismissible) return;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
  }
}
