#include "DictionaryDefinitionActivity.h"

#include <DictHtmlRenderer.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Utf8.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <memory>
#include <numeric>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DictFontUtils.h"
#include "util/Dictionary.h"
#include "util/DictionaryActivityUtils.h"
#include "util/TextPool.h"

static constexpr char kBullet[] = "- ";

void DictionaryDefinitionActivity::onEnter() {
  Activity::onEnter();
  LOG_DBG("DICT", "open word='%s' dict='%s' offset=%lu size=%lu", headword.c_str(), foundLocation.folderPath.c_str(),
          (unsigned long)foundLocation.offset, (unsigned long)foundLocation.size);
  wrapText();
  requestUpdate();
}

void DictionaryDefinitionActivity::onExit() { Activity::onExit(); }

int DictionaryDefinitionActivity::getLineHeight() const {
  return static_cast<int>(renderer.getLineHeight(SETTINGS.getDefinitionFontId()) *
                          SETTINGS.getDefinitionLineCompression());
}

void DictionaryDefinitionActivity::wrapText() {
  currentPage = 0;

  const auto orient = renderer.getOrientation();
  const auto metrics = UITheme::getInstance().getMetrics();
  const bool isLandscapeCw = orient == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orient == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orient == GfxRenderer::Orientation::PortraitInverted;
  hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.sideButtonHintsWidth : 0;
  hintGutterHeight = isInverted ? (metrics.buttonHintsHeight + metrics.verticalSpacing) : 0;
  contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int sidePadding = metrics.contentSidePadding + SETTINGS.screenMargin;
  leftPadding = contentX + sidePadding;
  rightPadding = (isLandscapeCcw ? hintGutterWidth : 0) + sidePadding;
  bodyStartY = hintGutterHeight + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  const int topArea = hintGutterHeight + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottomArea = metrics.buttonHintsHeight + metrics.verticalSpacing;

  linesPerPage = (renderer.getScreenHeight() - topArea - bottomArea) / getLineHeight();
  if (linesPerPage < 1) linesPerPage = 1;

  loadPage(currentPage);
}

void DictionaryDefinitionActivity::loadPage(int page) {
  layoutLines.clear();
  layoutLines.reserve(static_cast<size_t>(linesPerPage) + 1);
  pagePool_.clear();
  collectTargetPage_ = page;
  collectLineCount_ = 0;

  const DictInfo info = Dictionary::readInfo(foundLocation.folderPath.c_str());
  if (info.valid && info.sametypesequence[0] == 'h') {
    wrapHtml();
  } else {
    wrapPlain();
  }

  totalPages = DictLayout::paginate(collectLineCount_, linesPerPage);
}

void DictionaryDefinitionActivity::collectLineSink(void* ctx, DictLayout::LayoutLine&& line) {
  auto* self = static_cast<DictionaryDefinitionActivity*>(ctx);
  const int idx = self->collectLineCount_++;
  const int start = self->collectTargetPage_ * self->linesPerPage;
  if (idx < start || idx >= start + self->linesPerPage) return;

  PooledLine pooled;
  pooled.indentLevel = line.indentLevel;
  pooled.isListItem = line.isListItem;
  pooled.segments.reserve(line.segments.size());
  for (const auto& seg : line.segments) {
    PooledSegment ps;
    ps.offset = TextPool::append(self->pagePool_, seg.text.c_str(), seg.text.size());
    ps.len = static_cast<uint16_t>(seg.text.size());
    ps.style = seg.style;
    ps.isDictFont = seg.isDictFont;
    pooled.segments.push_back(ps);
  }
  self->layoutLines.push_back(std::move(pooled));
}

int DictionaryDefinitionActivity::getMixedWidth(std::vector<DictTextSpan>& dictRuns, const char* text,
                                                EpdFontFamily::Style style) {
  DictLayout::Measurer meas{this, &DictionaryDefinitionActivity::measureWidthAdapter};
  return DictLayout::getMixedWidth(dictRuns, text, style, meas);
}

int DictionaryDefinitionActivity::measureWidthAdapter(void* ctx, const char* text, EpdFontFamily::Style style,
                                                      bool isDictFont) {
  auto* self = static_cast<DictionaryDefinitionActivity*>(ctx);
  const int fontId = isDictFont ? DICT_FONT_ID : SETTINGS.getDefinitionFontId();
  if (!isDictFont && text[0] == ' ' && text[1] == '\0') return self->renderer.getSpaceWidth(fontId, style);
  return self->renderer.getTextAdvanceX(fontId, text, style);
}

void DictionaryDefinitionActivity::wrapHtml() {
  const int maxWidth = renderer.getScreenWidth() - leftPadding - rightPadding;
  const int indentStep = renderer.getTextWidth(SETTINGS.getDefinitionFontId(), "   ");
  const int bulletWidth = renderer.getTextWidth(SETTINGS.getDefinitionFontId(), kBullet);

  DictLayout::Measurer measure{this, &DictionaryDefinitionActivity::measureWidthAdapter};
  DictLayout::LineSink lineSink{this, &DictionaryDefinitionActivity::collectLineSink};
  DictLayout::Wrapper wrapper(DictLayout::WrapMetrics{maxWidth, indentStep, bulletWidth}, measure, lineSink);

  const std::string dictPath = foundLocation.folderPath + ".dict";
  const DictHtmlRenderer::SpanSink spanSink{&wrapper, &DictionaryDefinitionActivity::feedSpanToWrapper};
  htmlRenderer_.renderFromFileStreaming(dictPath.c_str(), foundLocation.offset, foundLocation.size, spanSink);
  wrapper.finish();
}

void DictionaryDefinitionActivity::feedSpanToWrapper(void* ctx, const StyledSpan& span) {
  static_cast<DictLayout::Wrapper*>(ctx)->onSpan(span);
}

void DictionaryDefinitionActivity::wrapPlain() {
  std::vector<DictTextSpan> dictRuns;
  const int screenWidth = renderer.getScreenWidth();
  const int maxWidth = screenWidth - leftPadding - rightPadding;
  const int spaceWidth = renderer.getSpaceWidth(SETTINGS.getDefinitionFontId(), EpdFontFamily::REGULAR);

  std::string currentWord;
  std::string currentLineText;
  int currentLineWidth = 0;

  DictLayout::LineSink sink{this, &DictionaryDefinitionActivity::collectLineSink};
  auto flushLine = [&]() {
    if (currentLineText.empty()) return;
    DictLayout::LayoutLine line;
    dictRuns.clear();
    splitDictRuns(currentLineText.c_str(), dictRuns);
    for (const auto& run : dictRuns) {
      line.segments.push_back({run.text, EpdFontFamily::REGULAR, run.isDictFont});
    }
    sink(std::move(line));
    currentLineText.clear();
    currentLineWidth = 0;
  };

  auto tryAppendWord = [&]() {
    if (currentWord.empty()) return;
    const int wordWidth = getMixedWidth(dictRuns, currentWord.c_str(), EpdFontFamily::REGULAR);
    if (currentLineText.empty()) {
      currentLineText = currentWord;
      currentLineWidth = wordWidth;
    } else {
      const int testWidth = currentLineWidth + spaceWidth + wordWidth;
      if (testWidth <= maxWidth) {
        currentLineText += ' ';
        currentLineText += currentWord;
        currentLineWidth = testWidth;
      } else {
        flushLine();
        currentLineText = currentWord;
        currentLineWidth = wordWidth;
      }
    }
    currentWord.clear();
  };

  const std::string dictPath = foundLocation.folderPath + ".dict";
  HalFile dictFile;
  if (!Storage.openFileForRead("DICT", dictPath.c_str(), dictFile)) return;
  dictFile.seekSet(foundLocation.offset);

  uint32_t remaining = foundLocation.size;
  char chunk[512];

  while (remaining > 0) {
    uint32_t toRead = remaining < sizeof(chunk) ? remaining : static_cast<uint32_t>(sizeof(chunk));
    int n = dictFile.read(reinterpret_cast<uint8_t*>(chunk), static_cast<int>(toRead));
    if (n <= 0) break;
    remaining -= static_cast<uint32_t>(n);

    for (int ci = 0; ci < n; ci++) {
      char c = chunk[ci];
      if (c == '\n') {
        tryAppendWord();
        flushLine();
      } else if (c == ' ') {
        tryAppendWord();
      } else {
        currentWord += c;
      }
    }
  }

  tryAppendWord();
  flushLine();
  dictFile.close();
}

void DictionaryDefinitionActivity::loop() {
  const bool shortPageTurnPressed =
      ReaderUtils::shortPowerButtonActionTriggered(mappedInput, CrossPointSettings::SHORT_PWRBTN::PAGE_TURN);

  const bool prevPage = mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextPage = shortPageTurnPressed || mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Right);
  const bool longPressPageNav =
      mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS && (prevPage || nextPage) && !shortPageTurnPressed;

  if (longPressPageNav && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    SETTINGS.orientation = ReaderUtils::rotatedOrientationForNavigation(nextPage);
    SETTINGS.saveToFile();
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    wrapText();
    requestUpdate();
    return;
  }

  if (prevPage && currentPage > 0) {
    currentPage--;
    loadPage(currentPage);
    requestUpdate();
  }

  if (nextPage && currentPage < totalPages - 1) {
    currentPage++;
    loadPage(currentPage);
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    DictUtils::cancelAndFinish(*this);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    DictUtils::cancelAndFinish(*this);
    return;
  }
}

void DictionaryDefinitionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto metrics = UITheme::getInstance().getMetrics();
  const int indentStep = renderer.getTextWidth(SETTINGS.getDefinitionFontId(), "   ");

  // Header
  GUI.drawHeader(renderer,
                 Rect{contentX, hintGutterHeight + metrics.topPadding, renderer.getScreenWidth() - hintGutterWidth,
                      metrics.headerHeight},
                 headword.c_str());

  // Body
  const int lineHeight = getLineHeight();
  auto renderBody = [&]() {
    for (int i = 0; i < linesPerPage && i < static_cast<int>(layoutLines.size()); i++) {
      const PooledLine& line = layoutLines[i];
      const int y = bodyStartY + i * lineHeight;
      int x = leftPadding + line.indentLevel * indentStep;

      if (line.isListItem) {
        renderer.drawText(SETTINGS.getDefinitionFontId(), x, y, kBullet);
        x += renderer.getTextWidth(SETTINGS.getDefinitionFontId(), kBullet);
      }

      for (const auto& seg : line.segments) {
        const int segFontId = seg.isDictFont ? DICT_FONT_ID : SETTINGS.getDefinitionFontId();
        const char* segText = pagePool_.data() + seg.offset;
        renderer.drawText(segFontId, x, y, segText, true, seg.style);
        if ((seg.style & EpdFontFamily::UNDERLINE) != 0) {
          const int segWidth = renderer.getTextWidth(segFontId, segText, seg.style);
          const int underlineY = y + renderer.getFontAscenderSize(segFontId) + 2;
          renderer.drawLine(x, underlineY, x + segWidth, underlineY, true);
        }
        x += renderer.getTextAdvanceX(segFontId, segText, seg.style);
      }
    }
  };
  renderBody();

  // Pagination indicator and button hints
  if (totalPages > 1) {
    char pageInfo[16];
    snprintf(pageInfo, sizeof(pageInfo), "%d/%d", currentPage + 1, totalPages);
    int textWidth = renderer.getTextWidth(SMALL_FONT_ID, pageInfo);
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - rightPadding - textWidth,
                      renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing, pageInfo);
  }

  const char* btn2 = "";
  const char* btn3 = totalPages > 1 ? tr(STR_DIR_UP) : "";
  const char* btn4 = totalPages > 1 ? tr(STR_DIR_DOWN) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), btn2, btn3, btn4);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::renderAntiAliased(renderer, renderBody);
  }
}
