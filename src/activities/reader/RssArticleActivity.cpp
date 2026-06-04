#include "RssArticleActivity.h"

#include <BidiUtils.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

void RssArticleActivity::onEnter() {
  Activity::onEnter();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  requestUpdate();
}

void RssArticleActivity::onExit() {
  Activity::onExit();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  lines.clear();
}

void RssArticleActivity::loop() {
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    onGoHome(HomeMenuItem::RSS_READER);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    finish();
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  (void)fromTilt;
  if (prevTriggered && currentPage > 0) {
    currentPage--;
    requestUpdate();
  } else if (nextTriggered) {
    if (currentPage < totalPages - 1) {
      currentPage++;
      requestUpdate();
    } else {
      finish();
    }
  }
}

void RssArticleActivity::render(RenderLock&&) {
  if (!initialized) initializeLayout();

  if (lines.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_NO_ARTICLE_TEXT), true,
                              EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  renderer.clearScreen();
  renderPage();
}

void RssArticleActivity::initializeLayout() {
  fontId = SETTINGS.getReaderFontId();
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  marginTop += SETTINGS.screenMargin;
  marginLeft += SETTINGS.screenMargin;
  marginRight += SETTINGS.screenMargin;
  marginBottom += std::max(SETTINGS.screenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  viewportWidth = renderer.getScreenWidth() - marginLeft - marginRight;
  const int viewportHeight = renderer.getScreenHeight() - marginTop - marginBottom;
  linesPerPage = std::max(1, viewportHeight / renderer.getLineHeight(fontId));

  buildLines();
  totalPages = std::max(1, static_cast<int>((lines.size() + linesPerPage - 1) / linesPerPage));
  initialized = true;
}

std::string RssArticleActivity::articleText() const {
  std::string text;
  if (!item.title.empty()) text += item.title + "\n\n";
  if (!item.author.empty() || !item.published.empty()) {
    if (!item.author.empty()) text += item.author;
    if (!item.author.empty() && !item.published.empty()) text += " - ";
    if (!item.published.empty()) text += item.published;
    text += "\n\n";
  }
  if (!item.content.empty()) {
    text += item.content;
  } else if (!item.link.empty()) {
    text += item.link;
  }
  return text;
}

void RssArticleActivity::buildLines() {
  lines.clear();
  std::string text = articleText();
  std::replace(text.begin(), text.end(), '\r', '\n');

  size_t start = 0;
  while (start <= text.size() && static_cast<int>(lines.size()) < MAX_ARTICLE_LINES) {
    size_t end = text.find('\n', start);
    std::string paragraph = end == std::string::npos ? text.substr(start) : text.substr(start, end - start);

    while (!paragraph.empty() && paragraph.front() == ' ') paragraph.erase(paragraph.begin());
    while (!paragraph.empty() && paragraph.back() == ' ') paragraph.pop_back();

    if (paragraph.empty()) {
      if (!lines.empty() && !lines.back().empty()) lines.emplace_back();
    } else {
      if (renderer.isSdCardFont(fontId)) renderer.ensureSdCardFontReady(fontId, paragraph.c_str(), 0x01);
      auto wrapped = renderer.wrappedText(fontId, paragraph.c_str(), viewportWidth,
                                          MAX_ARTICLE_LINES - static_cast<int>(lines.size()));
      lines.insert(lines.end(), wrapped.begin(), wrapped.end());
    }

    if (end == std::string::npos) break;
    start = end + 1;
  }
}

void RssArticleActivity::renderPage() {
  const int lineHeight = renderer.getLineHeight(fontId);
  const int startLine = currentPage * linesPerPage;
  const int endLine = std::min(static_cast<int>(lines.size()), startLine + linesPerPage);

  auto renderLines = [&]() {
    int y = marginTop;
    for (int i = startLine; i < endLine; i++) {
      const auto& line = lines[i];
      if (!line.empty()) {
        int x = marginLeft;
        uint8_t effectiveAlignment = SETTINGS.paragraphAlignment;
        const bool lineIsRtl = BidiUtils::startsWithRtl(line.c_str(), BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH);
        if (lineIsRtl &&
            (effectiveAlignment == CrossPointSettings::LEFT_ALIGN || effectiveAlignment == CrossPointSettings::JUSTIFIED)) {
          effectiveAlignment = CrossPointSettings::RIGHT_ALIGN;
        }
        const int textWidth = renderer.getTextAdvanceX(fontId, line.c_str(), EpdFontFamily::REGULAR);
        if (effectiveAlignment == CrossPointSettings::CENTER_ALIGN) {
          x = marginLeft + (viewportWidth - textWidth) / 2;
        } else if (effectiveAlignment == CrossPointSettings::RIGHT_ALIGN) {
          x = marginLeft + viewportWidth - textWidth;
        }
        renderer.drawText(fontId, x, y, line.c_str());
      }
      y += lineHeight;
    }
  };

  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  renderLines();
  scope.endScanAndPrewarm();

  renderLines();
  std::string title;
  if (SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE) title = item.title;
  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0;
  GUI.drawStatusBar(renderer, progress, currentPage + 1, totalPages, title);
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::renderAntiAliased(renderer, [&renderLines]() { renderLines(); });
  }
}
