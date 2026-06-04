#include "ClipSelectionActivity.h"

#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "../ActivityResult.h"
#include "MappedInputManager.h"
#include "clippings/ClipTextBuilder.h"
#include "components/UITheme.h"

ClipSelectionActivity::ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::vector<WordRef> words, std::string bookTitle, std::string author,
                                             std::string chapterTitle, int pageNumber, int fontId, Section& section,
                                             int startPageInSection, int marginTop, int marginLeft, Config config)
    : Activity("ClipSelection", renderer, mappedInput),
      words(std::move(words)),
      bookTitle(std::move(bookTitle)),
      author(std::move(author)),
      chapterTitle(std::move(chapterTitle)),
      pageNumber(pageNumber),
      fontId(fontId),
      config(config),
      section(section),
      startPageInSection(startPageInSection),
      marginTop(marginTop),
      marginLeft(marginLeft) {}

void ClipSelectionActivity::onEnter() {
  Activity::onEnter();

  if (words.empty()) {
    LOG_ERR("CLIP", "No words available for selection");
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  savedSectionPage = section.currentPage;
  savedBufferSize = renderer.getBufferSize();
  savedBuffer = makeUniqueNoThrow<uint8_t[]>(savedBufferSize);
  if (!savedBuffer) {
    LOG_ERR("CLIP", "malloc failed: %u bytes", savedBufferSize);
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  // Re-render page 0 to get a clean framebuffer — the previous activity (menu)
  // may still be painted on screen when onEnter() runs.
  switchToPage(0);
  requestUpdate();
}

void ClipSelectionActivity::onExit() {
  section.currentPage = savedSectionPage;
  savedBuffer.reset();
  Activity::onExit();
}

void ClipSelectionActivity::loop() {
  const int total = static_cast<int>(words.size());

  if (SETTINGS.clipNavMode == CrossPointSettings::CLIP_NAV_DIRECTIONAL) {
    using Btn = MappedInputManager::Button;

    buttonNavigator.onRelease({Btn::Left}, [this] {
      if (cursorIdx == 0) return;
      const int prevPage = words[cursorIdx].pageIdx;
      cursorIdx = cursorIdx - 1;
      if (words[cursorIdx].pageIdx != prevPage) needsPageSwitch = true;
      requestUpdate();
    });
    buttonNavigator.onContinuous({Btn::Left}, [this] {
      if (cursorIdx == 0) return;
      const int prevPage = words[cursorIdx].pageIdx;
      cursorIdx = cursorIdx - 1;
      if (words[cursorIdx].pageIdx != prevPage) needsPageSwitch = true;
      requestUpdate();
    });

    buttonNavigator.onRelease({Btn::Right}, [this, total] {
      if (cursorIdx + 1 >= total) return;
      const int prevPage = words[cursorIdx].pageIdx;
      cursorIdx = cursorIdx + 1;
      if (words[cursorIdx].pageIdx != prevPage) needsPageSwitch = true;
      requestUpdate();
    });
    buttonNavigator.onContinuous({Btn::Right}, [this, total] {
      if (cursorIdx + 1 >= total) return;
      const int prevPage = words[cursorIdx].pageIdx;
      cursorIdx = cursorIdx + 1;
      if (words[cursorIdx].pageIdx != prevPage) needsPageSwitch = true;
      requestUpdate();
    });

    buttonNavigator.onRelease({Btn::Down}, [this] {
      const int prevPage = words[cursorIdx].pageIdx;
      const int next = lineEndForward(cursorIdx);
      if (next == cursorIdx) return;
      cursorIdx = next;
      if (words[cursorIdx].pageIdx != prevPage) needsPageSwitch = true;
      requestUpdate();
    });
    buttonNavigator.onContinuous({Btn::Down}, [this] {
      const int prevPage = words[cursorIdx].pageIdx;
      const int next = lineEndForward(cursorIdx);
      if (next == cursorIdx) return;
      cursorIdx = next;
      if (words[cursorIdx].pageIdx != prevPage) needsPageSwitch = true;
      requestUpdate();
    });

    buttonNavigator.onRelease({Btn::Up}, [this] {
      const int prevPage = words[cursorIdx].pageIdx;
      const int prev = lineEndBackward(cursorIdx);
      if (prev == cursorIdx) return;
      cursorIdx = prev;
      if (words[cursorIdx].pageIdx != prevPage) needsPageSwitch = true;
      requestUpdate();
    });
    buttonNavigator.onContinuous({Btn::Up}, [this] {
      const int prevPage = words[cursorIdx].pageIdx;
      const int prev = lineEndBackward(cursorIdx);
      if (prev == cursorIdx) return;
      cursorIdx = prev;
      if (words[cursorIdx].pageIdx != prevPage) needsPageSwitch = true;
      requestUpdate();
    });
  } else {
    buttonNavigator.onNextRelease([this, total] {
      if (cursorIdx + 1 >= total) return;
      const int prevPage = words[cursorIdx].pageIdx;
      cursorIdx = cursorIdx + 1;
      if (words[cursorIdx].pageIdx != prevPage) needsPageSwitch = true;
      requestUpdate();
    });
    buttonNavigator.onNextContinuous([this] {
      const int prevPage = words[cursorIdx].pageIdx;
      const int next = lineEndForward(cursorIdx);
      if (next == cursorIdx) return;
      cursorIdx = next;
      if (words[cursorIdx].pageIdx != prevPage) needsPageSwitch = true;
      requestUpdate();
    });

    buttonNavigator.onPreviousRelease([this] {
      if (cursorIdx == 0) return;
      const int prevPage = words[cursorIdx].pageIdx;
      cursorIdx = cursorIdx - 1;
      if (words[cursorIdx].pageIdx != prevPage) needsPageSwitch = true;
      requestUpdate();
    });
    buttonNavigator.onPreviousContinuous([this] {
      const int prevPage = words[cursorIdx].pageIdx;
      const int prev = lineEndBackward(cursorIdx);
      if (prev == cursorIdx) return;
      cursorIdx = prev;
      if (words[cursorIdx].pageIdx != prevPage) needsPageSwitch = true;
      requestUpdate();
    });
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (startMarkIdx == -1) {
      startMarkIdx = cursorIdx;
      requestUpdate();
    } else {
      const int from = std::min(startMarkIdx, cursorIdx);
      const int to = std::max(startMarkIdx, cursorIdx);

      if (config.mode == Config::Mode::WORD_SELECT) {
        // Assemble plain text only — no anchor building.
        std::string text;
        for (int i = from; i <= to; ++i) {
          const auto& w = words[i].text;
          const bool hasEm = w.size() >= 3 && static_cast<unsigned char>(w[0]) == 0xE2 &&
                             static_cast<unsigned char>(w[1]) == 0x80 && static_cast<unsigned char>(w[2]) == 0x83;
          const std::string wtext = hasEm ? w.substr(3) : w;
          if (!text.empty()) text += ' ';
          text += wtext;
        }
        setResult(WordSelectResult{std::move(text), from, to});
      } else {
        setResult(ClipTextBuilder::build(words, from, to, total, startPageInSection));
      }
      finish();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (startMarkIdx != -1) {
      startMarkIdx = -1;
      requestUpdate();
    } else {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    }
  }
}

void ClipSelectionActivity::render(RenderLock&&) {
  if (!savedBuffer) return;

  if (needsPageSwitch) {
    switchToPage(words[cursorIdx].pageIdx);
    needsPageSwitch = false;
  }

  // Restore the saved page framebuffer, then draw highlights on top
  memcpy(renderer.getFrameBuffer(), savedBuffer.get(), savedBufferSize);
  drawHighlights();

  if (config.render.showButtonHints) {
    const auto confirmLabel = startMarkIdx == -1 ? tr(STR_SELECT) : tr(STR_DONE);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer(config.render.refreshMode);
}

void ClipSelectionActivity::switchToPage(int pageIdx) {
  const int oldPage = section.currentPage;
  section.currentPage = startPageInSection + pageIdx;
  auto page = section.loadPageFromSectionFile();
  if (!page) {
    section.currentPage = oldPage;
    LOG_ERR("CLIP", "Failed to load page %d (section.currentPage=%d, currentDisplayPage=%d) — reverted", pageIdx,
            section.currentPage, currentDisplayPage);
    return;
  }

  renderer.clearScreen();
  page->render(renderer, fontId, marginLeft, marginTop);
  // displayBuffer is intentionally omitted here — render() always controls the final display call
  memcpy(savedBuffer.get(), renderer.getFrameBuffer(), savedBufferSize);
  currentDisplayPage = pageIdx;
}

void ClipSelectionActivity::applyWordStyle(const WordRef& word, const WordStyle& style) const {
  const auto s = static_cast<EpdFontFamily::Style>(word.style & ~EpdFontFamily::UNDERLINE);
  const bool hasEm = word.text.size() >= 3 && static_cast<unsigned char>(word.text[0]) == 0xE2 &&
                     static_cast<unsigned char>(word.text[1]) == 0x80 &&
                     static_cast<unsigned char>(word.text[2]) == 0x83;
  const int skipX = hasEm ? renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", s) : 0;
  const int drawX = word.x + skipX;
  const int drawW = word.w - skipX;
  if (drawW <= 0) return;

  const uint8_t flags = style.flags;
  const bool doInvert = (flags & WordStyle::INVERT) != 0;
  const bool doFill = !doInvert && (flags & WordStyle::FILL) != 0;

  if (doInvert) {
    renderer.fillRect(drawX, word.y, drawW, word.h, true);
  } else if (doFill) {
    renderer.fillRectDither(drawX, word.y, drawW, word.h, style.fillColor);
  }

  if ((flags & WordStyle::BORDER) != 0) {
    renderer.drawRect(drawX, word.y, drawW, word.h, !doInvert);
  }

  if (word.text.find_first_not_of(" \t") != std::string::npos) {
    const bool textBlack = !doInvert;
    if (hasEm) {
      renderer.drawText(fontId, drawX, word.y, word.text.c_str() + 3, textBlack, s);
    } else {
      renderer.drawText(fontId, word.x, word.y, word.text.c_str(), textBlack, s);
    }
  }

  if ((flags & WordStyle::UNDERLINE) != 0) {
    const int underlineY = word.y + renderer.getFontAscenderSize(fontId) + 2;
    renderer.drawLine(drawX, underlineY, drawX + drawW, underlineY, true);
  }
}

void ClipSelectionActivity::drawHighlights() {
  if (startMarkIdx != -1) {
    const int from = std::min(startMarkIdx, cursorIdx);
    const int to = std::max(startMarkIdx, cursorIdx);

    // Render continuous same-row runs as a single highlight span
    int runStart = -1;
    for (int i = from; i <= to; ++i) {
      const bool skipWord = (words[i].pageIdx != currentDisplayPage);
      if (skipWord) {
        if (runStart >= 0) {
          // draw run [runStart, i-1] word by word
          for (int j = runStart; j <= i - 1; ++j) applyWordStyle(words[j], config.render.selection);
          runStart = -1;
        }
      } else if (runStart < 0 || words[i].y != words[runStart].y) {
        if (runStart >= 0) {
          for (int j = runStart; j <= i - 1; ++j) applyWordStyle(words[j], config.render.selection);
        }
        runStart = i;
      }
    }
    if (runStart >= 0) {
      for (int j = runStart; j <= to; ++j) applyWordStyle(words[j], config.render.selection);
    }
  }

  const auto& cw = words[cursorIdx];
  if (cw.pageIdx == currentDisplayPage) {
    applyWordStyle(cw, config.render.cursor);
  }
}

int ClipSelectionActivity::lineEndForward(int idx) const {
  const int total = static_cast<int>(words.size());
  const int lineY = words[idx].y;
  const int page = words[idx].pageIdx;
  for (int i = idx + 1; i < total; ++i) {
    if (words[i].pageIdx != page || words[i].y != lineY) return i;
  }
  return idx;
}

int ClipSelectionActivity::lineEndBackward(int idx) const {
  const int lineY = words[idx].y;
  const int page = words[idx].pageIdx;
  int i;
  for (i = idx - 1; i >= 0; --i) {
    if (words[i].pageIdx != page || words[i].y != lineY) break;
  }
  if (i < 0) return idx;
  const int prevY = words[i].y;
  const int prevPage = words[i].pageIdx;
  int first = i;
  for (; i >= 0; --i) {
    if (words[i].pageIdx != prevPage || words[i].y != prevY) break;
    first = i;
  }
  return first;
}
