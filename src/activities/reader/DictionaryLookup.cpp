#include "DictionaryLookup.h"

#include <Epub/blocks/TextBlock.h>
#include <Epub/hyphenation/HyphenationCommon.h>
#include <HalFileDictSource.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <iterator>

namespace {

// UTF-8 length of a codepoint, for slicing trimmed words out of the original.
size_t utf8CodepointLen(const uint32_t cp) {
  if (cp < 0x80) return 1;
  if (cp < 0x800) return 2;
  if (cp < 0x10000) return 3;
  return 4;
}

// Strips surrounding punctuation and footnote markers ("word," -> "word").
std::string trimmedWord(const char* text) {
  auto cps = collectCodepoints(std::string(text));
  trimSurroundingPunctuationAndFootnote(cps);
  if (cps.empty()) {
    return {};
  }
  const size_t start = cps.front().byteOffset;
  const size_t end = cps.back().byteOffset + utf8CodepointLen(cps.back().value);
  return std::string(text + start, end - start);
}

}  // namespace

bool DictionaryLookup::enter(std::unique_ptr<Page> newPage, const int marginLeft, const int marginTop,
                             const int readerFontId) {
  reset();
  if (!newPage) {
    return false;
  }
  page = std::move(newPage);
  fontId = readerFontId;
  buildWordBoxes(marginLeft, marginTop);
  if (words.empty()) {
    reset();
    return false;
  }
  selected = 0;
  state = State::SELECTING;
  drawHighlight();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  return true;
}

void DictionaryLookup::reset() {
  state = State::INACTIVE;
  words.clear();
  page.reset();
  highlightSnapshot.reset();
  popupSnapshot.reset();
  popupHeader.clear();
  popupLines.clear();
  popupScroll = 0;
  fullRenderNeeded = false;
  movesSinceGhostClean = 0;
  dictStore.close();
  // HalFile::close() asserts on a never-opened file; reset() runs on every
  // reader render, long before any dictionary file is opened.
  if (dictFile.isOpen()) {
    dictFile.close();
  }
  dictOpenFailed = false;
}

void DictionaryLookup::buildWordBoxes(const int marginLeft, const int marginTop) {
  const int lineHeight = renderer.getLineHeight(fontId);
  words.reserve(256);
  uint16_t lineIndex = 0;
  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) {
      continue;
    }
    const auto& line = static_cast<const PageLine&>(*element);
    const auto& block = line.getBlock();
    if (!block) {
      continue;
    }
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      const char* text = block->wordText(i);
      if (text == nullptr || text[0] == '\0') {
        continue;
      }
      WordBox box;
      box.text = text;
      // line.yPos is the y drawText receives, which is the TOP of the text
      // line (TextBlock draws the underline at y + ascender + 2), not the
      // baseline — so no ascender correction here.
      box.x = static_cast<int16_t>(marginLeft + line.xPos + block->wordXpos(i));
      box.y = static_cast<int16_t>(marginTop + line.yPos);
      box.w = static_cast<int16_t>(renderer.getTextWidth(fontId, text, block->wordStyle(i)));
      box.h = static_cast<int16_t>(lineHeight);
      box.lineIndex = lineIndex;
      if (box.w > 0) {
        words.push_back(box);
      }
    }
    lineIndex++;
  }
}

DictionaryLookup::TickResult DictionaryLookup::handleInput() {
  if (state == State::POPUP || state == State::MESSAGE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!closePopup()) {
        // The page under the popup could not be restored (snapshot OOM):
        // leave the mode and let the reader repaint the whole page.
        reset();
        return TickResult::EXITED_NEEDS_RENDER;
      }
      return TickResult::NONE;
    }
    if (state == State::POPUP && popupLines.size() > static_cast<size_t>(popupVisibleLines)) {
      const int maxScroll = static_cast<int>(popupLines.size()) - popupVisibleLines;
      int newScroll = popupScroll;
      if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
          mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
        newScroll = std::min(popupScroll + popupVisibleLines, maxScroll);
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                 mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
        newScroll = std::max(popupScroll - popupVisibleLines, 0);
      }
      if (newScroll != popupScroll) {
        popupScroll = newScroll;
        drawPopupContents();
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      }
    }
    return TickResult::NONE;
  }

  // SELECTING
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    eraseHighlight();
    const TickResult result = fullRenderNeeded ? TickResult::EXITED_NEEDS_RENDER : TickResult::EXITED;
    reset();
    if (result == TickResult::EXITED) {
      // HALF refresh on exit clears any ghosting the fast highlight updates left behind.
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    return result;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    lookupSelectedWord();
    return TickResult::NONE;
  }

  int wordDelta = 0, lineDelta = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
    wordDelta = 1;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
             mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
    wordDelta = -1;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    lineDelta = 1;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    lineDelta = -1;
  }
  if (wordDelta != 0 || lineDelta != 0) {
    moveSelection(wordDelta, lineDelta);
  }
  return TickResult::NONE;
}

void DictionaryLookup::moveSelection(const int wordDelta, const int lineDelta) {
  int target = selected;
  if (wordDelta != 0) {
    target = selected + wordDelta;
    if (target < 0 || target >= static_cast<int>(words.size())) {
      return;  // stay put at page bounds
    }
  } else {
    // Line navigation: find the nearest word (by horizontal center) on the
    // closest line with words in the requested direction.
    const WordBox& current = words[selected];
    const int currentCenter = current.x + current.w / 2;
    int bestDistance = -1;
    for (int i = 0; i < static_cast<int>(words.size()); i++) {
      const WordBox& candidate = words[i];
      const bool correctSide =
          lineDelta > 0 ? candidate.lineIndex > current.lineIndex : candidate.lineIndex < current.lineIndex;
      if (!correctSide) {
        continue;
      }
      const int lineDistance = std::abs(static_cast<int>(candidate.lineIndex) - static_cast<int>(current.lineIndex));
      const int centerDistance = std::abs(candidate.x + candidate.w / 2 - currentCenter);
      const int distance = lineDistance * 10000 + centerDistance;
      if (bestDistance < 0 || distance < bestDistance) {
        bestDistance = distance;
        target = i;
      }
    }
    if (target == selected) {
      return;  // no line in that direction
    }
  }

  eraseHighlight();
  selected = target;
  drawHighlight();
  // Thin outlines under FAST refresh accumulate ghosting; a periodic HALF
  // refresh wipes the residue without making every move slow.
  movesSinceGhostClean++;
  if (movesSinceGhostClean >= GHOST_CLEAN_EVERY_MOVES) {
    movesSinceGhostClean = 0;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

void DictionaryLookup::drawHighlight() {
  const WordBox& box = words[selected];
  const int x = box.x - HIGHLIGHT_PADDING;
  const int y = box.y - HIGHLIGHT_PADDING;
  const int w = box.w + 2 * HIGHLIGHT_PADDING;
  const int h = box.h + 2 * HIGHLIGHT_PADDING;
  if (snapshotRegion(x, y, w, h, highlightSnapshot)) {
    highlightRect[0] = x;
    highlightRect[1] = y;
    highlightRect[2] = w;
    highlightRect[3] = h;
  }
  renderer.drawRect(x, y, w, h, HIGHLIGHT_LINE_WIDTH, true);
}

void DictionaryLookup::eraseHighlight() {
  restoreRegion(highlightRect[0], highlightRect[1], highlightRect[2], highlightRect[3], highlightSnapshot);
}

bool DictionaryLookup::ensureDictionaryOpen() {
  if (dictStore.isOpen()) {
    return true;
  }
  if (dictOpenFailed) {
    return false;
  }

  HalFile dir = Storage.open(DICTIONARY_DIR);
  if (dir && dir.isDirectory()) {
    while (true) {
      HalFile entry = dir.openNextFile();
      if (!entry) {
        break;
      }
      char name[64] = {0};
      entry.getName(name, sizeof(name));
      const size_t len = strlen(name);
      // Skip directories, hidden files and macOS "._*" AppleDouble siblings,
      // and keep scanning past files that fail to parse — Finder copies leave
      // junk next to the real dictionary.
      if (entry.isDirectory() || name[0] == '.' || len <= 4 || strcasecmp(name + len - 4, ".cpd") != 0) {
        continue;
      }
      char path[96];
      snprintf(path, sizeof(path), "%s/%s", DICTIONARY_DIR, name);
      if (!Storage.openFileForRead("DICT", path, dictFile)) {
        LOG_ERR("DICT", "Cannot open %s", path);
        continue;
      }
      if (dictStore.open(halFileDictSource(dictFile))) {
        LOG_INF("DICT", "Opened dictionary: %s (%s)", path, dictStore.title());
        return true;
      }
      LOG_ERR("DICT", "Invalid dictionary file: %s", path);
      dictFile.close();
    }
  }
  LOG_ERR("DICT", "No usable .cpd dictionary in %s", DICTIONARY_DIR);
  dictOpenFailed = true;
  return false;
}

void DictionaryLookup::lookupSelectedWord() {
  if (!ensureDictionaryOpen()) {
    showMessage(StrId::STR_DICT_NO_DICTIONARY);
    return;
  }
  const std::string word = trimmedWord(words[selected].text);
  if (word.empty()) {
    showMessage(StrId::STR_DICT_NO_DEFINITION);
    return;
  }
  std::string definition;
  char matchedKey[DictionaryStore::KEY_LEN + 1] = {0};
  if (!dictStore.lookup(word.c_str(), definition, matchedKey)) {
    showMessage(StrId::STR_DICT_NO_DEFINITION);
    return;
  }
  openPopup(matchedKey, definition);
}

void DictionaryLookup::drawPopupFrame(const int popupX, const int popupY, const int popupW, const int popupH) {
  if (snapshotRegion(popupX, popupY, popupW, popupH, popupSnapshot)) {
    popupRect[0] = popupX;
    popupRect[1] = popupY;
    popupRect[2] = popupW;
    popupRect[3] = popupH;
  }
  renderer.fillRect(popupX, popupY, popupW, popupH, false);
  renderer.drawRect(popupX, popupY, popupW, popupH, 2, true);
}

void DictionaryLookup::openPopup(const char* header, const std::string& body) {
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const int lineHeight = renderer.getLineHeight(fontId);
  const int popupW = screenWidth - 2 * POPUP_MARGIN;
  const int textW = popupW - 2 * POPUP_PADDING;

  // Wrap paragraph by paragraph: wrappedText() does width wrapping only.
  popupHeader = header;
  popupLines.clear();
  size_t start = 0;
  while (start <= body.size() && popupLines.size() < MAX_DEFINITION_LINES) {
    size_t end = body.find('\n', start);
    if (end == std::string::npos) {
      end = body.size();
    }
    const std::string paragraph = body.substr(start, end - start);
    if (paragraph.empty()) {
      popupLines.emplace_back();
    } else {
      auto wrapped = renderer.wrappedText(fontId, paragraph.c_str(), textW,
                                          static_cast<int>(MAX_DEFINITION_LINES - popupLines.size()));
      popupLines.insert(popupLines.end(), std::make_move_iterator(wrapped.begin()),
                        std::make_move_iterator(wrapped.end()));
    }
    start = end + 1;
  }
  while (!popupLines.empty() && popupLines.back().empty()) {
    popupLines.pop_back();
  }

  // Height: header + separator gap + body lines, clamped to the screen.
  const int headerH = lineHeight + POPUP_PADDING;
  const int maxPopupH = screenHeight - 2 * POPUP_MARGIN;
  const int wantedH = 2 * POPUP_PADDING + headerH + static_cast<int>(popupLines.size()) * lineHeight;
  const int popupH = std::min(wantedH, maxPopupH);
  popupVisibleLines = std::max((popupH - 2 * POPUP_PADDING - headerH) / lineHeight, 1);
  popupScroll = 0;

  const int popupX = POPUP_MARGIN;
  const int popupY = (screenHeight - popupH) / 2;
  drawPopupFrame(popupX, popupY, popupW, popupH);
  state = State::POPUP;
  drawPopupContents();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void DictionaryLookup::drawPopupContents() {
  const int popupX = popupRect[0], popupY = popupRect[1], popupW = popupRect[2], popupH = popupRect[3];
  const int lineHeight = renderer.getLineHeight(fontId);
  const int textX = popupX + POPUP_PADDING;
  // drawText's y is the TOP of the text line, not the baseline.
  const int headerTop = popupY + POPUP_PADDING;

  // Clear the interior (keep the border) and draw header + separator.
  renderer.fillRect(popupX + 2, popupY + 2, popupW - 4, popupH - 4, false);
  renderer.drawText(fontId, textX, headerTop, popupHeader.c_str(), true, EpdFontFamily::BOLD);
  const int separatorY = headerTop + lineHeight + POPUP_PADDING / 4;
  renderer.drawLine(textX, separatorY, popupX + popupW - POPUP_PADDING, separatorY, true);

  int y = separatorY + POPUP_PADDING - POPUP_PADDING / 4;
  const int lastLine = std::min(popupScroll + popupVisibleLines, static_cast<int>(popupLines.size()));
  for (int i = popupScroll; i < lastLine; i++) {
    if (!popupLines[i].empty()) {
      renderer.drawText(fontId, textX, y, popupLines[i].c_str(), true);
    }
    y += lineHeight;
  }

  // Scroll indicators on the right edge.
  if (popupScroll > 0) {
    renderer.drawText(fontId, popupX + popupW - POPUP_PADDING - 8, headerTop, "^", true);
  }
  if (lastLine < static_cast<int>(popupLines.size())) {
    renderer.drawText(fontId, popupX + popupW - POPUP_PADDING - 8, popupY + popupH - POPUP_PADDING - lineHeight, "v",
                      true);
  }
}

void DictionaryLookup::showMessage(const StrId messageId) {
  openPopup(tr(STR_DICTIONARY), std::string(I18n::getInstance().get(messageId)));
  state = State::MESSAGE;
}

bool DictionaryLookup::closePopup() {
  const bool restorable = popupSnapshot != nullptr;
  if (restorable) {
    restoreRegion(popupRect[0], popupRect[1], popupRect[2], popupRect[3], popupSnapshot);
    // HALF refresh: the popup border/text would otherwise ghost over the page.
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
  popupHeader.clear();
  popupLines.clear();
  popupScroll = 0;
  state = State::SELECTING;
  return restorable;
}

bool DictionaryLookup::snapshotRegion(const int x, const int y, const int w, const int h,
                                      std::unique_ptr<uint8_t[]>& buf) {
  buf.reset();
  const size_t size = renderer.getRegionByteSize(x, y, w, h);
  if (size == 0) {
    fullRenderNeeded = true;
    return false;
  }
  auto snapshot = makeUniqueNoThrow<uint8_t[]>(size);
  if (!snapshot) {
    LOG_ERR("DICT", "OOM: %u byte region snapshot", static_cast<unsigned>(size));
    fullRenderNeeded = true;
    return false;
  }
  if (!renderer.copyRegionToBuffer(x, y, w, h, snapshot.get(), size)) {
    fullRenderNeeded = true;
    return false;
  }
  buf = std::move(snapshot);
  return true;
}

void DictionaryLookup::restoreRegion(const int x, const int y, const int w, const int h,
                                     std::unique_ptr<uint8_t[]>& buf) {
  if (!buf) {
    return;
  }
  const size_t size = renderer.getRegionByteSize(x, y, w, h);
  renderer.copyBufferToRegion(x, y, w, h, buf.get(), size);
  buf.reset();
}
