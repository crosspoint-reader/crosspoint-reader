#include "ClipSelectionActivity.h"

#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "../ActivityResult.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

ClipSelectionActivity::ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::vector<WordRef> words, std::string bookTitle, std::string author,
                                             std::string chapterTitle, int pageNumber, int fontId, Section& section,
                                             int startPageInSection, int marginTop, int marginLeft)
    : Activity("ClipSelection", renderer, mappedInput),
      words(std::move(words)),
      bookTitle(std::move(bookTitle)),
      author(std::move(author)),
      chapterTitle(std::move(chapterTitle)),
      pageNumber(pageNumber),
      fontId(fontId),
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
  savedBuffer = static_cast<uint8_t*>(malloc(savedBufferSize));
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
  free(savedBuffer);
  savedBuffer = nullptr;
  Activity::onExit();
}

void ClipSelectionActivity::loop() {
  const int total = static_cast<int>(words.size());

  buttonNavigator.onNextRelease([this, total] {
    if (cursorIdx + 1 >= total) return;
    const int prevPage = words[cursorIdx].pageIdx;
    cursorIdx = cursorIdx + 1;
    if (words[cursorIdx].pageIdx != prevPage) needsPageSwitch = true;
    requestUpdate();
  });

  if (SETTINGS.clipNavMode == CrossPointSettings::LINE_AWARE) {
    buttonNavigator.onNextContinuous([this] {
      const int prevPage = words[cursorIdx].pageIdx;
      const int next = lineEndForward(cursorIdx);
      if (next == cursorIdx) return;
      cursorIdx = next;
      if (words[cursorIdx].pageIdx != prevPage) needsPageSwitch = true;
      requestUpdate();
    });
  } else {
    buttonNavigator.onNextContinuous([this, total] {
      if (cursorIdx + 1 >= total) return;
      const int prevPage = words[cursorIdx].pageIdx;
      cursorIdx = cursorIdx + 1;
      if (words[cursorIdx].pageIdx != prevPage) needsPageSwitch = true;
      requestUpdate();
    });
  }

  buttonNavigator.onPreviousRelease([this] {
    if (cursorIdx == 0) return;
    const int prevPage = words[cursorIdx].pageIdx;
    cursorIdx = cursorIdx - 1;
    if (words[cursorIdx].pageIdx != prevPage) needsPageSwitch = true;
    requestUpdate();
  });

  if (SETTINGS.clipNavMode == CrossPointSettings::LINE_AWARE) {
    buttonNavigator.onPreviousContinuous([this] {
      const int prevPage = words[cursorIdx].pageIdx;
      const int prev = lineEndBackward(cursorIdx);
      if (prev == cursorIdx) return;
      cursorIdx = prev;
      if (words[cursorIdx].pageIdx != prevPage) needsPageSwitch = true;
      requestUpdate();
    });
  } else {
    buttonNavigator.onPreviousContinuous([this] {
      if (cursorIdx == 0) return;
      const int prevPage = words[cursorIdx].pageIdx;
      cursorIdx = cursorIdx - 1;
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
      std::string text;
      std::vector<ClippingResult::AnnotationRect> rects;
      rects.reserve(to - from + 1);
      auto pushWordRect = [this](int i, std::vector<ClippingResult::AnnotationRect>& out) {
        int rx = words[i].x;
        int rw = words[i].w;
        const auto& t = words[i].text;
        if (t.size() >= 3 && static_cast<unsigned char>(t[0]) == 0xE2 && static_cast<unsigned char>(t[1]) == 0x80 &&
            static_cast<unsigned char>(t[2]) == 0x83) {
          const auto s = static_cast<EpdFontFamily::Style>(words[i].style & ~EpdFontFamily::UNDERLINE);
          const int skip = renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", s);
          rx += skip;
          rw -= skip;
        }
        out.push_back({static_cast<int16_t>(rx), static_cast<int16_t>(words[i].y), static_cast<int16_t>(rw),
                       static_cast<int16_t>(words[i].h), static_cast<uint16_t>(startPageInSection + words[i].pageIdx)});
      };
      auto stripEmSpace = [](const std::string& w) -> const std::string& {
        static thread_local std::string buf;
        if (w.size() >= 3 && static_cast<unsigned char>(w[0]) == 0xE2 && static_cast<unsigned char>(w[1]) == 0x80 &&
            static_cast<unsigned char>(w[2]) == 0x83) {
          buf = w.substr(3);
          return buf;
        }
        return w;
      };
      auto hasEmSpace = [](const std::string& w) -> bool {
        return w.size() >= 3 && static_cast<unsigned char>(w[0]) == 0xE2 && static_cast<unsigned char>(w[1]) == 0x80 &&
               static_cast<unsigned char>(w[2]) == 0x83;
      };
      for (int i = from; i <= to; ++i) {
        const auto& wtext = stripEmSpace(words[i].text);
        const bool emSpaceStart = hasEmSpace(words[i].text) && i > from;
        const bool yGapBreak =
            i > from && words[i].pageIdx == words[i - 1].pageIdx && words[i].y > words[i - 1].y + words[i - 1].h;
        const bool paragraphStart = emSpaceStart || yGapBreak;
        if (i > from && !text.empty() && !paragraphStart) {
          const auto& prev = words[i - 1].text;
          const auto& prevStripped = stripEmSpace(prev);
          if (prevStripped.size() >= 1 && prevStripped.back() == '-' && !wtext.empty() &&
              static_cast<unsigned char>(wtext[0]) >= 'a' && static_cast<unsigned char>(wtext[0]) <= 'z' &&
              prevStripped.find('-') == prevStripped.size() - 1) {
            text.pop_back();
            text += wtext;
            pushWordRect(i, rects);
            continue;
          }
        }
        if (paragraphStart) {
          text += '\n';
        } else if (!text.empty()) {
          text += ' ';
        }
        text += wtext;
        pushWordRect(i, rects);
      }

      ActivityResult result;
      result.data = ClippingResult{std::move(text), from, to, std::move(rects)};
      setResult(std::move(result));
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
  memcpy(renderer.getFrameBuffer(), savedBuffer, savedBufferSize);
  drawHighlights();

  const auto confirmLabel = startMarkIdx == -1 ? tr(STR_SELECT) : tr(STR_DONE);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
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
  memcpy(savedBuffer, renderer.getFrameBuffer(), savedBufferSize);
  currentDisplayPage = pageIdx;
}

void ClipSelectionActivity::drawHighlights() {
  auto hasEmSpace = [](const std::string& t) {
    return t.size() >= 3 && static_cast<unsigned char>(t[0]) == 0xE2 && static_cast<unsigned char>(t[1]) == 0x80 &&
           static_cast<unsigned char>(t[2]) == 0x83;
  };

  auto emSpaceSkip = [this, &hasEmSpace](const std::string& t, EpdFontFamily::Style s) -> int {
    if (hasEmSpace(t)) return renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", s);
    return 0;
  };

  auto drawContinuousHighlight = [this, &emSpaceSkip, &hasEmSpace](int first, int last) {
    const auto& fw = words[first];
    const auto& lw = words[last];
    const auto fs = static_cast<EpdFontFamily::Style>(fw.style & ~EpdFontFamily::UNDERLINE);
    const int skip = emSpaceSkip(fw.text, fs);
    const int startX = fw.x + skip;
    const int spanW = (lw.x + lw.w) - startX;
    renderer.fillRectDither(startX, fw.y, spanW, fw.h, Color::LightGray);
    for (int i = first; i <= last; ++i) {
      if (words[i].text.find_first_not_of(" \t") != std::string::npos) {
        const auto s = static_cast<EpdFontFamily::Style>(words[i].style & ~EpdFontFamily::UNDERLINE);
        if (i == first && hasEmSpace(words[i].text)) {
          renderer.drawText(fontId, startX, words[i].y, words[i].text.c_str() + 3, true, s);
        } else {
          renderer.drawText(fontId, words[i].x, words[i].y, words[i].text.c_str(), true, s);
        }
      }
    }
  };

  if (startMarkIdx != -1) {
    const int from = std::min(startMarkIdx, cursorIdx);
    const int to = std::max(startMarkIdx, cursorIdx);
    int runStart = -1;
    for (int i = from; i <= to; ++i) {
      bool skipWord = (words[i].pageIdx != currentDisplayPage);
      if (skipWord) {
        if (runStart >= 0) {
          drawContinuousHighlight(runStart, i - 1);
          runStart = -1;
        }
      } else if (runStart < 0 || words[i].y != words[runStart].y) {
        if (runStart >= 0) {
          drawContinuousHighlight(runStart, i - 1);
        }
        runStart = i;
      }
    }
    if (runStart >= 0) {
      drawContinuousHighlight(runStart, to);
    }
  }

  const auto& cw = words[cursorIdx];
  if (cw.pageIdx == currentDisplayPage) {
    const auto cs = static_cast<EpdFontFamily::Style>(cw.style & ~EpdFontFamily::UNDERLINE);
    const int skip = emSpaceSkip(cw.text, cs);
    const int cx = cw.x + skip;
    const int cWidth = cw.w - skip;
    if (cWidth > 0) {
      renderer.fillRectDither(cx, cw.y, cWidth, cw.h, Color::LightGray);
      if (cw.text.find_first_not_of(" \t") != std::string::npos) {
        if (hasEmSpace(cw.text)) {
          renderer.drawText(fontId, cx, cw.y, cw.text.c_str() + 3, true, cs);
        } else {
          renderer.drawText(fontId, cw.x, cw.y, cw.text.c_str(), true, cs);
        }
      }
    }
  }
}

ClipSelectionActivity::Rect ClipSelectionActivity::alignedRect(int x, int y, int w, int h) const {
  const int alignedX = (x / 8) * 8;
  const int alignedW = ((x + w + 7) / 8) * 8 - alignedX;
  return {alignedX, y, alignedW, h};
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
