#include "FileViewerActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include "components/UITheme.h"
#include "fontIds.h"

// ── helpers ──────────────────────────────────────────────────────────────────

std::string FileViewerActivity::shortName() const {
  const auto slash = filePath.rfind('/');
  return (slash == std::string::npos) ? filePath : filePath.substr(slash + 1);
}

// Wrap raw text into lines that fit within `maxWidth` pixels using the given font.
static void wrapText(const std::string& text, std::vector<std::string>& out,
                     const GfxRenderer& renderer, int fontId, int maxWidth) {
  std::string current;
  for (size_t i = 0; i <= text.size(); i++) {
    const char ch = (i < text.size()) ? text[i] : '\n';
    if (ch == '\r') continue;
    if (ch == '\n') {
      out.push_back(current);
      current.clear();
      continue;
    }
    current += ch;
    if (renderer.getTextWidth(fontId, current.c_str()) >= maxWidth - 2) {
      // Try to break at last space
      const auto sp = current.rfind(' ');
      if (sp != std::string::npos && sp > 0) {
        out.push_back(current.substr(0, sp));
        current = current.substr(sp + 1);
      } else {
        // Hard break
        out.push_back(current.substr(0, current.size() - 1));
        current = std::string(1, ch);
      }
    }
  }
}

// ── Activity lifecycle ────────────────────────────────────────────────────────

void FileViewerActivity::loadFile() {
  lines.clear();
  scrollLine = 0;

  FsFile f = Storage.open(filePath.c_str());
  if (!f) {
    lines.push_back("[could not open file]");
    return;
  }

  const size_t fileSize = f.size();
  const size_t readSize = (fileSize > MAX_BYTES) ? MAX_BYTES : fileSize;
  std::string text;
  text.resize(readSize);
  f.read(&text[0], readSize);
  f.close();

  if (fileSize > MAX_BYTES) {
    text += "\n[... truncated — showing first " + std::to_string(MAX_BYTES) + " bytes ...]";
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth  = renderer.getScreenWidth();
  const int maxW = pageWidth - metrics.contentSidePadding - 8;
  wrapText(text, lines, renderer, SMALL_FONT_ID, maxW);
  if (lines.empty()) lines.push_back("[empty file]");
}

void FileViewerActivity::onEnter() {
  Activity::onEnter();
  loadFile();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageHeight = renderer.getScreenHeight();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentH   = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int lineH      = renderer.getTextHeight(SMALL_FONT_ID) + 2;
  linesPerPage = (lineH > 0) ? (contentH / lineH) : 20;

  requestUpdate();
}

void FileViewerActivity::onExit() {
  Activity::onExit();
  lines.clear();
}

void FileViewerActivity::loop() {
  const int total = static_cast<int>(lines.size());

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  buttonNavigator.onNextRelease([this, total] {
    if (scrollLine + linesPerPage < total) {
      scrollLine += 1;
      requestUpdate();
    }
  });
  buttonNavigator.onPreviousRelease([this] {
    if (scrollLine > 0) {
      scrollLine -= 1;
      requestUpdate();
    }
  });
  buttonNavigator.onNextContinuous([this, total] {
    scrollLine = std::min(scrollLine + linesPerPage, std::max(0, total - linesPerPage));
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this] {
    scrollLine = std::max(0, scrollLine - linesPerPage);
    requestUpdate();
  });
}

void FileViewerActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth  = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics   = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 shortName().c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentH   = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int lineH      = renderer.getTextHeight(SMALL_FONT_ID) + 2;
  const int textX      = metrics.contentSidePadding;
  const int total      = static_cast<int>(lines.size());

  int y = contentTop;
  for (int i = scrollLine; i < total && (y + lineH) <= (contentTop + contentH); i++) {
    renderer.drawText(SMALL_FONT_ID, textX, y, lines[i].c_str(), /*black=*/true);
    y += lineH;
  }

  // Scroll position hint (e.g. "12/47")
  if (total > linesPerPage) {
    char pos[16];
    snprintf(pos, sizeof(pos), "%d/%d", scrollLine + 1, total);
    const int pw = renderer.getTextWidth(SMALL_FONT_ID, pos);
    renderer.drawText(SMALL_FONT_ID, pageWidth - pw - 4,
                      contentTop + contentH - renderer.getTextHeight(SMALL_FONT_ID) - 2,
                      pos, /*black=*/true);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
