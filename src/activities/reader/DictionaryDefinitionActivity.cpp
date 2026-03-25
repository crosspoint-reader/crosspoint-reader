#include "DictionaryDefinitionActivity.h"

#include <DictHtmlRenderer.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <memory>

#include "DictionarySuggestionsActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/LookupHistory.h"

static LookupHistory::Status toHistStatus(DictionaryLookupController::FoundStatus fs) {
  switch (fs) {
    case DictionaryLookupController::FoundStatus::Direct:
      return LookupHistory::Status::Direct;
    case DictionaryLookupController::FoundStatus::Stem:
      return LookupHistory::Status::Stem;
    case DictionaryLookupController::FoundStatus::AltForm:
      return LookupHistory::Status::AltForm;
    case DictionaryLookupController::FoundStatus::Suggestion:
      return LookupHistory::Status::Suggestion;
    default:
      return LookupHistory::Status::NotFound;
  }
}

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

std::string DictionaryDefinitionActivity::buildPhraseFromRange(int fromIdx, int toIdx) const {
  const int lo = std::min(fromIdx, toIdx);
  const int hi = std::max(fromIdx, toIdx);
  std::string phrase;
  for (int i = lo; i <= hi; i++) {
    const auto* w = navigator.getWordAt(i);
    if (!w) continue;
    if (!phrase.empty()) phrase += ' ';
    phrase += w->text;
  }
  return Dictionary::cleanWord(phrase);
}

void DictionaryDefinitionActivity::wrapText() {
  layoutLines.clear();
  isWordSelectMode = false;
  inMultiSelectMode = false;
  anchorFlatIndex = -1;
  navigator.reset();

  const auto orient = renderer.getOrientation();
  const auto metrics = UITheme::getInstance().getMetrics();
  const bool isLandscapeCw = orient == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orient == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orient == GfxRenderer::Orientation::PortraitInverted;
  hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.sideButtonHintsWidth : 0;
  hintGutterHeight = isInverted ? (metrics.buttonHintsHeight + metrics.verticalSpacing) : 0;
  contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int sidePadding = metrics.contentSidePadding;
  leftPadding = contentX + sidePadding;
  rightPadding = (isLandscapeCcw ? hintGutterWidth : 0) + sidePadding;
  bodyStartY = hintGutterHeight + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  const int lineHeight = renderer.getLineHeight(readerFontId);
  const int topArea = hintGutterHeight + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottomArea = metrics.buttonHintsHeight + metrics.verticalSpacing;

  linesPerPage = (renderer.getScreenHeight() - topArea - bottomArea) / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  // Choose rendering path based on dictionary content type
  const DictInfo info = Dictionary::readInfo(Dictionary::getActivePath());
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
  const int screenWidth = renderer.getScreenWidth();
  const int maxWidth = screenWidth - leftPadding - rightPadding;

  // Indent step: 3 spaces worth of pixels at regular weight
  const int indentStep = renderer.getTextWidth(readerFontId, "   ");
  const int bulletWidth = renderer.getTextWidth(readerFontId, "- ");

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

  auto appendToLine = [&](const std::string& text, EpdFontFamily::Style style, int width) {
    if (!currentLine.segments.empty() && currentLine.segments.back().style == style) {
      currentLine.segments.back().text += text;
    } else {
      currentLine.segments.push_back({text, style});
    }
    currentX += width;
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

    if (span.newlineBefore) {
      flushLine();
      startLine(span.indentLevel, span.isListItem);
    }

    int spanWidth = renderer.getTextWidth(readerFontId, span.text, style);
    if (currentX + spanWidth <= maxWidth) {
      // Fast path: entire span fits on the current line.
      appendToLine(std::string(span.text), style, spanWidth);
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
        int candidateWidth = renderer.getTextWidth(readerFontId, candidate.c_str(), style);

        if (currentX + candidateWidth > maxWidth && !lineIsEmpty) {
          flushLine();
          startLine(span.indentLevel, false);
          candidate = tok;
          candidateWidth = renderer.getTextWidth(readerFontId, tok.c_str(), style);
        }

        appendToLine(candidate, style, candidateWidth);
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
  const int screenWidth = renderer.getScreenWidth();
  const int maxWidth = screenWidth - leftPadding - rightPadding;

  std::string currentWord;
  std::string currentLineText;

  auto flushLine = [&]() {
    LayoutLine line;
    line.segments.push_back({currentLineText, EpdFontFamily::REGULAR});
    layoutLines.push_back(std::move(line));
    currentLineText.clear();
  };

  for (size_t i = 0; i <= definition.size(); i++) {
    char c = (i < definition.size()) ? definition[i] : '\0';

    if (c == '\n' || c == '\0') {
      if (!currentWord.empty()) {
        if (currentLineText.empty()) {
          currentLineText = currentWord;
        } else {
          std::string test = currentLineText + " " + currentWord;
          if (renderer.getTextWidth(readerFontId, test.c_str()) <= maxWidth) {
            currentLineText = test;
          } else {
            flushLine();
            currentLineText = currentWord;
          }
        }
        currentWord.clear();
      }
      flushLine();
    } else if (c == ' ') {
      if (!currentWord.empty()) {
        if (currentLineText.empty()) {
          currentLineText = currentWord;
        } else {
          std::string test = currentLineText + " " + currentWord;
          if (renderer.getTextWidth(readerFontId, test.c_str()) <= maxWidth) {
            currentLineText = test;
          } else {
            flushLine();
            currentLineText = currentWord;
          }
        }
        currentWord.clear();
      }
    } else {
      currentWord += c;
    }
  }
}

// ---------------------------------------------------------------------------
// Word-select: extract words from the currently visible page
// ---------------------------------------------------------------------------

void DictionaryDefinitionActivity::extractWordsFromLayout() {
  const int lineHeight = renderer.getLineHeight(readerFontId);
  const int indentStep = renderer.getTextWidth(readerFontId, "   ");
  const int spaceWidth = renderer.getTextWidth(readerFontId, " ");

  std::vector<WordSelectNavigator::WordInfo> words;
  std::vector<WordSelectNavigator::Row> rows;

  const int startLineIdx = currentPage * linesPerPage;
  for (int i = 0; i < linesPerPage && (startLineIdx + i) < static_cast<int>(layoutLines.size()); i++) {
    const LayoutLine& line = layoutLines[startLineIdx + i];
    const int16_t lineY = static_cast<int16_t>(bodyStartY + i * lineHeight);
    int x = leftPadding + line.indentLevel * indentStep;

    if (line.isListItem) {
      x += renderer.getTextWidth(readerFontId, "- ");
    }

    for (const auto& seg : line.segments) {
      const char* p = seg.text.c_str();
      while (*p) {
        while (*p == ' ') {
          x += spaceWidth;
          ++p;
        }
        if (!*p) break;

        const char* tokStart = p;
        while (*p && *p != ' ') ++p;
        std::string tok(tokStart, p - tokStart);

        const int tokWidth = renderer.getTextWidth(readerFontId, tok.c_str(), seg.style);
        std::string cleaned = Dictionary::cleanWord(tok);
        if (!cleaned.empty()) {
          words.push_back({tok, static_cast<int16_t>(x), lineY, static_cast<int16_t>(tokWidth), 0, seg.style});
          words.back().lookupText = cleaned;
        }
        x += tokWidth;
      }
    }
  }

  // Organise into rows by Y coordinate (each LayoutLine maps to one row)
  if (!words.empty()) {
    int16_t currentY = words[0].screenY;
    rows.push_back({currentY, {}});
    for (size_t i = 0; i < words.size(); i++) {
      if (words[i].screenY != currentY) {
        currentY = words[i].screenY;
        rows.push_back({currentY, {}});
      }
      words[i].row = static_cast<int16_t>(rows.size() - 1);
      rows.back().wordIndices.push_back(static_cast<int>(i));
    }
  }

  navigator.load(std::move(words), std::move(rows));
}

// ---------------------------------------------------------------------------
// Background lookup (for definition word-select)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Not-found helper: findSimilar → suggestions activity, or controller.setNotFound()
// On suggestion selected: re-trigger lookup via controller (in-place replace on FoundDefinition).
// ---------------------------------------------------------------------------

void DictionaryDefinitionActivity::handleNotFound(const std::string& word) {
  auto similar = Dictionary::findSimilar(word, 6);
  if (!similar.empty()) {
    startActivityForResult(
        std::make_unique<DictionarySuggestionsActivity>(renderer, mappedInput, std::move(similar), readerFontId),
        [this](const ActivityResult& result) {
          if (result.isCancelled) {
            controller.setNotFound();
            return;
          }
          const auto& wr = std::get<WordResult>(result.data);
          controller.startLookupAsSuggestion(wr.word);  // in-place replace on FoundDefinition
        });
    return;
  }
  if (!cachePath.empty()) {
    LookupHistory::addWord(cachePath, word, LookupHistory::Status::NotFound);
  }
  controller.setNotFound();
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
          LookupHistory::addWord(cachePath, controller.getLookupWord(), toHistStatus(controller.getFoundStatus()));
          chainDepth++;
        }
        chainBackNavInProgress = false;
        headword = controller.getFoundWord();
        definition = controller.getFoundDefinition();
        wrapText();
        currentPage = 0;
        isWordSelectMode = false;
        requestUpdate();
        break;
      case DictionaryLookupController::LookupEvent::LookupFailed:
        handleNotFound(controller.getLookupWord());
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
        inMultiSelectMode = false;
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

    if (inMultiSelectMode) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        const int cursorIdx = navigator.getCurrentFlatIndex();
        std::string phrase = buildPhraseFromRange(anchorFlatIndex, cursorIdx);
        if (phrase.empty()) {
          GUI.drawPopup(renderer, tr(STR_DICT_NO_WORD));
          renderer.displayBuffer(HalDisplay::FAST_REFRESH);
          vTaskDelay(1000 / portTICK_PERIOD_MS);
          requestUpdate();
          return;
        }
        inMultiSelectMode = false;
        controller.startLookup(phrase);
        return;
      }

      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        inMultiSelectMode = false;
        requestUpdate();
        return;
      }
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (mappedInput.getHeldTime() >= LONG_PRESS_MS) {
        const int flatIdx = navigator.getCurrentFlatIndex();
        if (flatIdx >= 0) {
          inMultiSelectMode = true;
          anchorFlatIndex = flatIdx;
          requestUpdate();
        }
        return;
      }
      const auto* sel = navigator.getSelected();
      if (!sel) return;
      if (sel->lookupText.empty()) return;
      controller.startLookup(sel->lookupText);
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      if (mappedInput.getHeldTime() >= LONG_PRESS_MS) {
        // Long press — Done: exit all the way to reader
        setResult(ActivityResult{});
        finish();
      } else {
        // Short press — exit word-select, return to definition view
        isWordSelectMode = false;
        inMultiSelectMode = false;
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

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (showLookupButton && mappedInput.getHeldTime() >= LONG_PRESS_MS) {
      setResult(ActivityResult{});  // Done: isCancelled=false — exit all the way
      finish();
      return;
    }
    if (!cachePath.empty() && chainDepth > 0) {
      chainDepth--;
      const int fileIdx = chainStartIndex - 1 + chainDepth;
      const std::string prevWord = LookupHistory::getWordAt(cachePath, fileIdx);
      if (!prevWord.empty()) {
        chainBackNavInProgress = true;
        controller.startLookup(prevWord);
        return;
      }
      // File read failed — fall through to normal exit
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
  const int lineHeight = renderer.getLineHeight(readerFontId);
  const int indentStep = renderer.getTextWidth(readerFontId, "   ");

  // Header
  GUI.drawHeader(renderer,
                 Rect{contentX, hintGutterHeight + metrics.topPadding, renderer.getScreenWidth() - hintGutterWidth,
                      metrics.headerHeight},
                 headword.c_str());

  // Body: draw layout lines for the current page
  const int startLine = currentPage * linesPerPage;
  for (int i = 0; i < linesPerPage && (startLine + i) < static_cast<int>(layoutLines.size()); i++) {
    const LayoutLine& line = layoutLines[startLine + i];
    const int y = bodyStartY + i * lineHeight;
    int x = leftPadding + line.indentLevel * indentStep;

    if (line.isListItem) {
      renderer.drawText(readerFontId, x, y, "- ");
      x += renderer.getTextWidth(readerFontId, "- ");
    }

    for (const auto& seg : line.segments) {
      renderer.drawText(readerFontId, x, y, seg.text.c_str(), true, seg.style);
      x += renderer.getTextWidth(readerFontId, seg.text.c_str(), seg.style);
    }
  }

  // Word-select mode: overlay highlighted word(s)
  if (isWordSelectMode) {
    if (inMultiSelectMode) {
      const int cursorIdx = navigator.getCurrentFlatIndex();
      const int lo = std::min(anchorFlatIndex, cursorIdx);
      const int hi = std::max(anchorFlatIndex, cursorIdx);
      for (int i = lo; i <= hi; i++) {
        const auto* w = navigator.getWordAt(i);
        if (!w) continue;
        renderer.fillRect(w->screenX - 2, w->screenY - 2, w->width + 4, lineHeight + 4, true);
        renderer.drawText(readerFontId, w->screenX, w->screenY, w->text.c_str(), false, w->style);
      }
    } else {
      if (const auto* sel = navigator.getSelected()) {
        renderer.fillRect(sel->screenX - 2, sel->screenY - 2, sel->width + 4, lineHeight + 4, true);
        renderer.drawText(readerFontId, sel->screenX, sel->screenY, sel->text.c_str(), false, sel->style);
      }
    }
    // Empty button hints in word-select mode (same convention as EPUB word-select)
    const auto labels = mappedInput.mapLabels("", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  // View mode: pagination indicator and button hints
  if (totalPages > 1) {
    std::string pageInfo = std::to_string(currentPage + 1) + "/" + std::to_string(totalPages);
    int textWidth = renderer.getTextWidth(SMALL_FONT_ID, pageInfo.c_str());
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - rightPadding - textWidth,
                      renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing,
                      pageInfo.c_str());
  }

  const char* btn2 = showLookupButton ? tr(STR_LOOKUP_SHORT) : "";
  const char* btn3 = totalPages > 1 ? tr(STR_PREV_NEXT) : "";
  const char* btn4 = totalPages > 1 ? tr(STR_NEXT_PREV) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), btn2, btn3, btn4);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
