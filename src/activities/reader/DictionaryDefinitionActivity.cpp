#include "DictionaryDefinitionActivity.h"

#include <DictHtmlRenderer.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Utf8.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <memory>
#include <numeric>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/IpaUtils.h"
#include "util/LookupHistory.h"

static constexpr char kBullet[] = "- ";

void DictionaryDefinitionActivity::onEnter() {
  Activity::onEnter();
  wrapText();
  requestUpdate();
}

void DictionaryDefinitionActivity::onExit() {
  controller.onExit();
  Activity::onExit();
}

// ---------------------------------------------------------------------------
// Layout helpers — shared setup
// ---------------------------------------------------------------------------

void DictionaryDefinitionActivity::wrapText() {
  layoutLines.clear();
  layoutLines.reserve(32);
  isWordSelectMode = false;
  navigator.reset();

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

  const int lineHeight = static_cast<int>(renderer.getLineHeight(SETTINGS.getDefinitionFontId()) *
                                          SETTINGS.getDefinitionLineCompression());
  const int topArea = hintGutterHeight + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottomArea = metrics.buttonHintsHeight + metrics.verticalSpacing;

  linesPerPage = (renderer.getScreenHeight() - topArea - bottomArea) / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  // Choose rendering path based on dictionary content type
  const std::string folderPath = Dictionary::readDictPath(cachePath.empty() ? nullptr : cachePath.c_str());
  const DictInfo info = Dictionary::readInfo(folderPath.c_str());
  if (info.valid && info.sametypesequence[0] == 'h') {
    wrapHtml();
  } else {
    wrapPlain();
  }

  totalPages = (static_cast<int>(layoutLines.size()) + linesPerPage - 1) / linesPerPage;
  if (totalPages < 1) totalPages = 1;
}

// ---------------------------------------------------------------------------
// HTML path: run DictHtmlRenderer, lay out spans into LayoutLines
// ---------------------------------------------------------------------------

void DictionaryDefinitionActivity::wrapHtml() {
  std::vector<IpaTextSpan> ipaRuns;
  const int screenWidth = renderer.getScreenWidth();
  const int maxWidth = screenWidth - leftPadding - rightPadding;

  // Indent step: 3 spaces worth of pixels at regular weight
  const int indentStep = renderer.getTextWidth(SETTINGS.getDefinitionFontId(), "   ");
  const int bulletWidth = renderer.getTextWidth(SETTINGS.getDefinitionFontId(), kBullet);

  // Heap-allocate the renderer — textBuf[8192] is too large for the stack
  auto htmlRenderer = std::make_unique<DictHtmlRenderer>();
  const auto& spans = htmlRenderer->render(definition.c_str(), static_cast<int>(definition.size()));

  LayoutLine currentLine;
  int currentX = 0;

  auto flushLine = [&]() {
    if (!currentLine.segments.empty()) {
      layoutLines.push_back(std::move(currentLine));
      currentLine = LayoutLine{};
    }
  };

  auto startLine = [&](uint8_t indent, bool listItem) {
    currentLine.indentLevel = indent;
    currentLine.isListItem = listItem;
    currentX = indent * indentStep + (listItem ? bulletWidth : 0);
  };

  auto appendToLine = [&](const std::string& text, EpdFontFamily::Style style, bool isIpa, int width) {
    if (!currentLine.segments.empty() && currentLine.segments.back().style == style &&
        currentLine.segments.back().isIpa == isIpa) {
      currentLine.segments.back().text += text;
    } else {
      currentLine.segments.push_back({text, style, isIpa});
    }
    currentX += width;
  };

  // Measure and append a string that may contain mixed IPA/non-IPA runs.
  auto getMixedWidth = [&](const char* text, EpdFontFamily::Style style) -> int {
    ipaRuns.clear();
    splitIpaRuns(text, ipaRuns);
    return std::accumulate(ipaRuns.begin(), ipaRuns.end(), 0, [&](int sum, const IpaTextSpan& run) {
      return sum +
             renderer.getTextWidth(run.isIpa ? IPA_FONT_ID : SETTINGS.getDefinitionFontId(), run.text.c_str(), style);
    });
  };

  auto appendMixed = [&](const char* text, EpdFontFamily::Style style) {
    ipaRuns.clear();
    splitIpaRuns(text, ipaRuns);
    for (const auto& run : ipaRuns) {
      const int fontId = run.isIpa ? IPA_FONT_ID : SETTINGS.getDefinitionFontId();
      appendToLine(run.text, style, run.isIpa, renderer.getTextWidth(fontId, run.text.c_str(), style));
    }
  };

  // Break a single token at codepoint boundaries when it is wider than the available line width.
  auto breakToken = [&](const std::string& tok, EpdFontFamily::Style style, uint8_t indentLevel) {
    const auto* bp = reinterpret_cast<const uint8_t*>(tok.c_str());
    std::string pending;
    int pendingWidth = 0;
    uint32_t cp;
    while ((cp = utf8NextCodepoint(&bp))) {
      char buf[5] = {};
      if (cp < 0x80) {
        buf[0] = static_cast<char>(cp);
      } else if (cp < 0x800) {
        buf[0] = static_cast<char>(0xC0 | (cp >> 6));
        buf[1] = static_cast<char>(0x80 | (cp & 0x3F));
      } else if (cp < 0x10000) {
        buf[0] = static_cast<char>(0xE0 | (cp >> 12));
        buf[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = static_cast<char>(0x80 | (cp & 0x3F));
      } else {
        buf[0] = static_cast<char>(0xF0 | (cp >> 18));
        buf[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = static_cast<char>(0x80 | (cp & 0x3F));
      }
      const int cpLen = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
      std::string cpStr(buf, cpLen);
      const int fontId = isIpaCodepoint(cp) ? IPA_FONT_ID : SETTINGS.getDefinitionFontId();
      const int cpWidth = renderer.getTextWidth(fontId, cpStr.c_str(), style);
      if (!pending.empty() && currentX + pendingWidth + cpWidth > maxWidth) {
        appendMixed(pending.c_str(), style);
        flushLine();
        startLine(indentLevel, false);
        pending.clear();
        pendingWidth = 0;
      }
      pending += cpStr;
      pendingWidth += cpWidth;
    }
    if (!pending.empty()) appendMixed(pending.c_str(), style);
  };

  startLine(0, false);

  for (const auto& span : spans) {
    if (!span.text || span.text[0] == '\0') continue;

    EpdFontFamily::Style style;
    if (span.bold && span.italic) {
      style = EpdFontFamily::BOLD_ITALIC;
    } else if (span.bold) {
      style = EpdFontFamily::BOLD;
    } else if (span.italic) {
      style = EpdFontFamily::ITALIC;
    } else {
      style = EpdFontFamily::REGULAR;
    }
    if (span.underline) style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::UNDERLINE);

    if (span.newlineBefore) {
      flushLine();
      startLine(span.indentLevel, span.isListItem);
    }

    const int spanWidth = getMixedWidth(span.text, style);
    if (currentX + spanWidth <= maxWidth) {
      // Fast path: entire span fits on the current line.
      appendMixed(span.text, style);
    } else {
      // Word-wrap within the span.
      const char* p = span.text;
      while (*p) {
        bool hadSpace = false;
        while (*p == ' ') {
          hadSpace = true;
          ++p;
        }
        if (!*p) break;

        const char* tokStart = p;
        while (*p && *p != ' ') ++p;
        std::string tok(tokStart, p - tokStart);

        bool lineIsEmpty = currentLine.segments.empty();
        std::string candidate = (!lineIsEmpty && hadSpace) ? " " + tok : tok;
        int candidateWidth = getMixedWidth(candidate.c_str(), style);

        if (currentX + candidateWidth > maxWidth && !lineIsEmpty) {
          flushLine();
          startLine(span.indentLevel, false);
          candidate = tok;
          candidateWidth = getMixedWidth(tok.c_str(), style);
        }

        if (currentX + candidateWidth > maxWidth) {
          breakToken(candidate, style, span.indentLevel);
        } else {
          appendMixed(candidate.c_str(), style);
        }
      }
    }
  }

  flushLine();
  // htmlRenderer freed here; span text has been copied into layoutLines
}

// ---------------------------------------------------------------------------
// Plain text path: word-wrap into single-segment REGULAR lines
// ---------------------------------------------------------------------------

void DictionaryDefinitionActivity::wrapPlain() {
  std::vector<IpaTextSpan> ipaRuns;
  const int screenWidth = renderer.getScreenWidth();
  const int maxWidth = screenWidth - leftPadding - rightPadding;
  const int spaceWidth = renderer.getSpaceWidth(SETTINGS.getDefinitionFontId(), EpdFontFamily::REGULAR);

  std::string currentWord;
  std::string currentLineText;
  int currentLineWidth = 0;

  auto getMixedWidthPlain = [&](const std::string& text) -> int {
    ipaRuns.clear();
    splitIpaRuns(text.c_str(), ipaRuns);
    return std::accumulate(ipaRuns.begin(), ipaRuns.end(), 0, [&](int sum, const IpaTextSpan& run) {
      return sum + renderer.getTextWidth(run.isIpa ? IPA_FONT_ID : SETTINGS.getDefinitionFontId(), run.text.c_str());
    });
  };

  auto flushLine = [&]() {
    if (currentLineText.empty()) return;
    LayoutLine line;
    ipaRuns.clear();
    splitIpaRuns(currentLineText.c_str(), ipaRuns);
    for (const auto& run : ipaRuns) {
      line.segments.push_back({run.text, EpdFontFamily::REGULAR, run.isIpa});
    }
    layoutLines.push_back(std::move(line));
    currentLineText.clear();
    currentLineWidth = 0;
  };

  auto tryAppendWord = [&]() {
    if (currentWord.empty()) return;
    const int wordWidth = getMixedWidthPlain(currentWord);
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

  for (size_t i = 0; i <= definition.size(); i++) {
    char c = (i < definition.size()) ? definition[i] : '\0';
    if (c == '\n' || c == '\0') {
      tryAppendWord();
      flushLine();
    } else if (c == ' ') {
      tryAppendWord();
    } else {
      currentWord += c;
    }
  }
}

// ---------------------------------------------------------------------------
// Word-select: extract words from the currently visible page
// ---------------------------------------------------------------------------

void DictionaryDefinitionActivity::extractWordsFromLayout() {
  const int lineHeight = static_cast<int>(renderer.getLineHeight(SETTINGS.getDefinitionFontId()) *
                                          SETTINGS.getDefinitionLineCompression());
  const int indentStep = renderer.getTextWidth(SETTINGS.getDefinitionFontId(), "   ");

  std::vector<WordSelectNavigator::WordInfo> words;
  words.reserve(64);
  std::vector<WordSelectNavigator::Row> rows;
  rows.reserve(16);
  std::string textPool;
  textPool.reserve(512);

  const int startLineIdx = currentPage * linesPerPage;
  for (int i = 0; i < linesPerPage && (startLineIdx + i) < static_cast<int>(layoutLines.size()); i++) {
    const LayoutLine& line = layoutLines[startLineIdx + i];
    const int16_t lineY = static_cast<int16_t>(bodyStartY + i * lineHeight);
    int x = leftPadding + line.indentLevel * indentStep;

    if (line.isListItem) {
      x += renderer.getTextWidth(SETTINGS.getDefinitionFontId(), kBullet);
    }

    for (const auto& seg : line.segments) {
      const int segFontId = seg.isIpa ? IPA_FONT_ID : SETTINGS.getDefinitionFontId();
      const int spaceWidth = renderer.getSpaceWidth(segFontId, seg.style);
      const char* p = seg.text.c_str();
      while (*p) {
        while (*p == ' ') {
          x += spaceWidth;
          ++p;
        }
        if (!*p) break;

        const char* tokStart = p;
        while (*p && *p != ' ') ++p;
        const size_t tokLen = static_cast<size_t>(p - tokStart);
        std::string tok(tokStart, tokLen);

        const int tokVisualWidth = renderer.getTextWidth(segFontId, tok.c_str(), seg.style);
        const int tokAdvanceX = renderer.getTextAdvanceX(segFontId, tok.c_str(), seg.style);
        std::string cleaned = Dictionary::cleanWord(tok);
        if (!cleaned.empty()) {
          uint16_t tokOff = WordSelectNavigator::poolAppend(textPool, tok.c_str(), tok.size());
          uint16_t cleanedOff = WordSelectNavigator::poolAppend(textPool, cleaned.c_str(), cleaned.size());
          WordSelectNavigator::WordInfo wi;
          wi.textOffset = tokOff;
          wi.textLen = static_cast<uint16_t>(tok.size());
          wi.lookupOffset = cleanedOff;
          wi.lookupLen = static_cast<uint16_t>(cleaned.size());
          wi.screenX = static_cast<int16_t>(x);
          wi.screenY = lineY;
          wi.width = static_cast<int16_t>(tokVisualWidth);
          wi.style = seg.style;
          wi.isIpa = seg.isIpa;
          wi.fontId = segFontId;
          words.push_back(wi);
        }
        x += tokAdvanceX;
      }
    }
  }

  WordSelectNavigator::organizeIntoRows(words, rows);
  navigator.load(std::move(words), std::move(rows), std::move(textPool));
}

// ---------------------------------------------------------------------------
// Input loop
// ---------------------------------------------------------------------------

void DictionaryDefinitionActivity::loop() {
  // --- Controller active (LookingUp / AltFormPrompt / NotFound) ---
  if (controller.isActive()) {
    switch (controller.handleInput()) {
      case DictionaryLookupController::LookupEvent::FoundDefinition:
        if (!cachePath.empty() && !chainBackNavInProgress) {
          LookupHistory::addWord(cachePath, controller.getLookupWord(),
                                 DictionaryLookupController::toHistStatus(controller.getFoundStatus()));
          chainWords.push_back(headword);
        }
        chainBackNavInProgress = false;
        headword = controller.getFoundWord();
        definition = controller.getFoundDefinition();
        wrapText();
        currentPage = 0;
        isWordSelectMode = false;
        requestUpdate();
        break;
      case DictionaryLookupController::LookupEvent::NotFoundDismissedBack:
        requestUpdate();
        break;
      case DictionaryLookupController::LookupEvent::NotFoundDismissedDone:
        setResult(ActivityResult{});
        finish();
        break;
      case DictionaryLookupController::LookupEvent::Cancelled:
        isWordSelectMode = false;
        navigator.reset();
        requestUpdate();
        break;
      default:
        break;
    }
    return;
  }

  // --- Word-select mode ---
  if (isWordSelectMode) {
    if (navigator.handleNavigation(mappedInput, renderer)) {
      requestUpdate();
    }

    if (controller.handleMultiSelect(navigator)) return;

    if (!navigator.isMultiSelecting()) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        const auto* sel = navigator.getSelected();
        if (!sel) return;
        controller.lookupOrPopup(navigator.getLookup(*sel));
        return;
      }

      // Long press Back: exit-all (fire at threshold).
      if (mappedInput.isPressed(MappedInputManager::Button::Back) &&
          mappedInput.getHeldTime() >= Dictionary::LONG_PRESS_MS) {
        setResult(ActivityResult{});
        finish();
        return;
      }

      // Short press Back: exit word-select mode.
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
          mappedInput.getHeldTime() < Dictionary::LONG_PRESS_MS) {
        isWordSelectMode = false;
        navigator.reset();
        requestUpdate();
      }
    }
    return;
  }

  // --- View mode ---
  const bool prevPage = mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextPage = mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Right);

  if (prevPage && currentPage > 0) {
    currentPage--;
    requestUpdate();
  }

  if (nextPage && currentPage < totalPages - 1) {
    currentPage++;
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (showLookupButton) {
      extractWordsFromLayout();
      if (!navigator.isEmpty()) {
        isWordSelectMode = true;
        requestUpdate();
      }
    } else {
      ActivityResult r;
      r.isCancelled = true;
      setResult(std::move(r));
      finish();
    }
    return;
  }

  // Long press Back: exit-all (fire at threshold, only when lookup button shown).
  if (showLookupButton && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= Dictionary::LONG_PRESS_MS) {
    setResult(ActivityResult{});  // Done: isCancelled=false — exit all the way
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      (!showLookupButton || mappedInput.getHeldTime() < Dictionary::LONG_PRESS_MS)) {
    if (!cachePath.empty() && !chainWords.empty()) {
      std::string prevWord = chainWords.back();
      chainWords.pop_back();
      chainBackNavInProgress = true;
      controller.startLookup(prevWord);
      return;
    }
    ActivityResult r;
    r.isCancelled = true;
    setResult(std::move(r));
    finish();
    return;
  }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void DictionaryDefinitionActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (controller.render()) return;

  const auto metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = static_cast<int>(renderer.getLineHeight(SETTINGS.getDefinitionFontId()) *
                                          SETTINGS.getDefinitionLineCompression());
  const int indentStep = renderer.getTextWidth(SETTINGS.getDefinitionFontId(), "   ");

  // Header
  GUI.drawHeader(renderer,
                 Rect{contentX, hintGutterHeight + metrics.topPadding, renderer.getScreenWidth() - hintGutterWidth,
                      metrics.headerHeight},
                 headword.c_str());

  // Body: draw layout lines for the current page (BW pass)
  const int startLine = currentPage * linesPerPage;
  auto renderBody = [&]() {
    for (int i = 0; i < linesPerPage && (startLine + i) < static_cast<int>(layoutLines.size()); i++) {
      const LayoutLine& line = layoutLines[startLine + i];
      const int y = bodyStartY + i * lineHeight;
      int x = leftPadding + line.indentLevel * indentStep;

      if (line.isListItem) {
        renderer.drawText(SETTINGS.getDefinitionFontId(), x, y, kBullet);
        x += renderer.getTextWidth(SETTINGS.getDefinitionFontId(), kBullet);
      }

      for (const auto& seg : line.segments) {
        const int segFontId = seg.isIpa ? IPA_FONT_ID : SETTINGS.getDefinitionFontId();
        renderer.drawText(segFontId, x, y, seg.text.c_str(), true, seg.style);
        if ((seg.style & EpdFontFamily::UNDERLINE) != 0) {
          const int segWidth = renderer.getTextWidth(segFontId, seg.text.c_str(), seg.style);
          const int underlineY = y + renderer.getFontAscenderSize(segFontId) + 2;
          renderer.drawLine(x, underlineY, x + segWidth, underlineY, true);
        }
        x += renderer.getTextAdvanceX(segFontId, seg.text.c_str(), seg.style);
      }
    }
  };
  renderBody();

  // Word-select mode: overlay highlighted word(s)
  if (isWordSelectMode) {
    navigator.renderHighlight(renderer, lineHeight);
    // Empty button hints in word-select mode (same convention as EPUB word-select)
    const auto labels = mappedInput.mapLabels("", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  // View mode: pagination indicator and button hints
  if (totalPages > 1) {
    char pageInfo[16];
    snprintf(pageInfo, sizeof(pageInfo), "%d/%d", currentPage + 1, totalPages);
    int textWidth = renderer.getTextWidth(SMALL_FONT_ID, pageInfo);
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - rightPadding - textWidth,
                      renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing, pageInfo);
  }

  const char* btn2 = showLookupButton ? tr(STR_LOOKUP_SHORT) : "";
  const char* btn3 = totalPages > 1 ? tr(STR_DIR_UP) : "";
  const char* btn4 = totalPages > 1 ? tr(STR_DIR_DOWN) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), btn2, btn3, btn4);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  // Anti-aliasing pass: overlay grayscale body text on top of the BW display
  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::renderAntiAliased(renderer, renderBody);
  }
}
