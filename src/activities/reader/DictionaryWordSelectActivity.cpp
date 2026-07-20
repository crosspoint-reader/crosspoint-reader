#include "DictionaryWordSelectActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cctype>
#include <climits>
#include <cstdlib>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "components/UITheme.h"

namespace {

constexpr unsigned long POPUP_DURATION_MS = 1500;

// A token is selectable when it has an ASCII alphanumeric or a non-ASCII
// codepoint outside U+2000-U+206F (dashes, bullets and other General
// Punctuation that appear as standalone tokens are not words).
bool isSelectableToken(const char* text) {
  for (const uint8_t* p = reinterpret_cast<const uint8_t*>(text); *p != 0; p++) {
    if (*p < 0x80) {
      if (std::isalnum(*p)) return true;
    } else if (*p == 0xE2 && (p[1] == 0x80 || p[1] == 0x81)) {
      if (p[2] == 0) break;  // truncated sequence: skipping would step past the NUL
      p += 2;                // skip the 3-byte General Punctuation codepoint
    } else {
      return true;
    }
  }
  return false;
}

void indexBuildYield(void*) { vTaskDelay(1); }

}  // namespace

void DictionaryWordSelectActivity::onEnter() {
  Activity::onEnter();
  fontId = SETTINGS.getReaderFontId();
  lineHeight = renderer.getLineHeight(fontId);
  // No null check: a failed allocation just disables the differential
  // fast path (drawHighlightWithSnapshot skips the read), keeping the
  // full-repaint path as the fallback.
  snapshot = makeUniqueNoThrow<uint8_t[]>(SNAPSHOT_CAPACITY);
  extractWords();
  // Start on the middle row's word nearest mid-screen instead of top-left:
  // any word on the page is then at most half a page of moves away.
  if (!words.empty()) {
    const int initial = closestInRow(rowCount / 2, renderer.getScreenWidth() / 2);
    if (initial >= 0) selected = initial;
  }
  requestUpdate();
}

void DictionaryWordSelectActivity::extractWords() {
  words.clear();
  words.reserve(128);
  rowCount = 0;

  // Single walk: collect the selectable words while accumulating their text
  // and styles (~2KB transient string, freed on return). Widths are measured
  // afterwards: merging the page's codepoints into the SD font's persistent
  // advance table first keeps getTextAdvanceX on the in-RAM path instead of
  // loading glyphs from SD one overflow slot at a time.
  std::string pageText;
  pageText.reserve(2048);
  uint8_t styleMask = 0;

  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block || !block->valid()) continue;

    bool rowHasWords = false;
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      const char* text = block->wordText(i);
      if (!isSelectableToken(text)) continue;

      WordBox box;
      box.x = static_cast<int16_t>(line->xPos + block->wordXpos(i) + marginLeft);
      box.y = static_cast<int16_t>(line->yPos + marginTop);
      box.style = block->wordStyle(i);
      box.width = 0;  // measured below, once the advance table is ready
      box.row = rowCount;
      box.text = text;
      words.push_back(box);
      rowHasWords = true;

      pageText.append(text);
      pageText.push_back(' ');
      styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(box.style) & 0x03));
    }
    if (rowHasWords) rowCount++;
  }

  if (styleMask == 0) styleMask = 0x01;  // REGULAR
  renderer.ensureSdCardFontReady(fontId, pageText.c_str(), styleMask);
  for (auto& word : words) {
    word.width = static_cast<int16_t>(renderer.getTextAdvanceX(fontId, word.text, word.style));
  }
}

// Index of the word whose box (with finger-sized slop) contains the touch
// point; -1 when the touch lands on no word. Boxes never overlap after the
// slop grows them, at worst they touch, so first hit wins.
int DictionaryWordSelectActivity::wordAt(const int x, const int y) const {
  constexpr int SLOP = 4;  // matches the highlight box (+2) plus finger error
  for (int i = 0; i < static_cast<int>(words.size()); i++) {
    const WordBox& word = words[i];
    if (x >= word.x - SLOP && x < word.x + word.width + SLOP && y >= word.y - SLOP && y < word.y + lineHeight + SLOP) {
      return i;
    }
  }
  return -1;
}

// Index of the word in `row` whose horizontal center is closest to centerX;
// -1 when the row has no words.
int DictionaryWordSelectActivity::closestInRow(const uint16_t row, const int centerX) const {
  int best = -1;
  int bestDistance = INT_MAX;
  for (int i = 0; i < static_cast<int>(words.size()); i++) {
    if (words[i].row != row) continue;
    const int distance = std::abs(words[i].x + words[i].width / 2 - centerX);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return best;
}

void DictionaryWordSelectActivity::moveVertical(const int direction) {
  const WordBox& current = words[selected];
  const int targetRow = static_cast<int>(current.row) + direction;
  if (targetRow < 0 || targetRow >= static_cast<int>(rowCount)) return;

  const int best = closestInRow(static_cast<uint16_t>(targetRow), current.x + current.width / 2);
  if (best >= 0 && best != selected) {
    selected = best;
    requestUpdate();
  }
}

void DictionaryWordSelectActivity::performLookup() {
  popup = Popup::Busy;
  if (!dictOpenAttempted) {
    dictOpenAttempted = true;
    dictOpenOk = dict.open(SETTINGS.dictionaryName);
  }
  const bool indexing = dictOpenOk && dict.needsIndex();
  popupMsg = indexing ? StrId::STR_DICT_INDEXING : StrId::STR_DICT_LOOKING_UP;
  requestUpdateAndWait();  // paint the page + busy popup before blocking on SD

  bool ok = dictOpenOk;
  if (ok && indexing) ok = dict.buildIndex(&indexBuildYield);

  std::string definition;
  std::string headword;
  const bool found = ok && dict.lookup(words[selected].text, definition, headword);

  if (found) {
    popup = Popup::None;
    startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, std::move(headword),
                                                                          std::move(definition)),
                           [this](const ActivityResult&) { requestUpdate(); });
    return;
  }
  popup = ok ? Popup::NotFound : Popup::Error;
  popupMsg = ok ? StrId::STR_DICT_NOT_FOUND : StrId::STR_DICT_ERROR;
  popupTime = millis();
  requestUpdate();
}

void DictionaryWordSelectActivity::loop() {
  if (popup == Popup::NotFound || popup == Popup::Error) {
    if (millis() - popupTime >= POPUP_DURATION_MS) {
      popup = Popup::None;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmPressSeen = true;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && confirmPressSeen && !words.empty()) {
    performLookup();
    return;
  }

  if (words.empty()) return;

  // Touch: a touch-down moves the highlight to the touched word (differential
  // repaint), a tap on a word selects and looks it up in one go.
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTouchDown(tx, ty)) {
    const int hit = wordAt(tx, ty);
    if (hit >= 0 && hit != selected) {
      selected = hit;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasScreenTapped(tx, ty)) {
    const int hit = wordAt(tx, ty);
    if (hit >= 0) {
      selected = hit;
      performLookup();
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Left) && selected > 0) {
    selected--;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Right) &&
             selected + 1 < static_cast<int>(words.size())) {
    selected++;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    moveVertical(-1);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    moveVertical(1);
  }
}

// Saves the pixels under words[selected]'s highlight box, then draws the
// highlight over them. Returns false when the pixels could not be saved
// (no buffer / oversize box) — the highlight is drawn regardless, but the
// next cursor move must do a full repaint.
bool DictionaryWordSelectActivity::drawHighlightWithSnapshot() {
  const WordBox& word = words[selected];
  int hx = word.x - 2;
  int hy = word.y - 2;
  int hw = word.width + 4;
  int hh = lineHeight + 4;
  // Clamp to the panel so save, draw and restore all use the same box.
  if (hx < 0) {
    hw += hx;
    hx = 0;
  }
  if (hy < 0) {
    hh += hy;
    hy = 0;
  }

  bool saved = false;
  if (snapshot && hw > 0 && hh > 0) {
    saved = renderer.readFramebufferRegion(hx, hy, hw, hh, snapshot.get(), SNAPSHOT_CAPACITY) > 0;
  }
  snapshotX = static_cast<int16_t>(hx);
  snapshotY = static_cast<int16_t>(hy);
  snapshotW = static_cast<int16_t>(hw);
  snapshotH = static_cast<int16_t>(hh);
  snapshotIdx = saved ? selected : -1;

  renderer.fillRect(hx, hy, hw, hh, true);
  renderer.drawText(fontId, word.x, word.y, word.text, false, word.style);
  return saved;
}

// Front-button bar (Back/Confirm/Left/Right). Drawn last on every repaint
// path, including the differential highlight-only path, so it always ends
// up as the top layer even when a highlighted word's box falls under a
// hint's screen area. No side-button hints: Up/Down row jump has no spare
// screen area on this page (it reuses the reader's full-bleed layout), and
// a hint box there would hide text instead of sitting in a reserved gutter.
void DictionaryWordSelectActivity::drawHints() const {
  // No selectable word on this page: Confirm/Left/Right are all no-ops
  // (guarded by words.empty() in loop()/performLookup), so only Back does
  // anything and only Back is hinted.
  if (words.empty()) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_LOOKUP), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void DictionaryWordSelectActivity::render(RenderLock&&) {
  // Differential fast path: only the highlight moved and the framebuffer
  // still holds a clean page (no popup or sub-activity since the last full
  // repaint). Restore the pixels under the old highlight, draw the new one,
  // and push — skipping the two-pass page render entirely.
  if (popup == Popup::None && snapshotIdx >= 0 && !words.empty() && selected != snapshotIdx) {
    renderer.writeFramebufferRegion(snapshotX, snapshotY, snapshotW, snapshotH, snapshot.get());
    // The full path's PrewarmScope cleared the glyph cache on exit; batch-load
    // just the highlighted word's glyphs before drawing them white-on-black.
    renderer.getFontCacheManager()->prewarmCache(
        fontId, words[selected].text, static_cast<uint8_t>(1u << (static_cast<uint8_t>(words[selected].style) & 0x03)));
    if (drawHighlightWithSnapshot()) {
      drawHints();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return;
    }
    // Snapshot failed (oversize box) — fall through to a full repaint.
  }

  renderer.clearScreen();

  // Same prewarm-scan-then-render pass the reader uses, so SD-card fonts hit
  // the in-RAM glyph cache during the real draw.
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, fontId, marginLeft, marginTop);
  scope.endScanAndPrewarm();
  page->render(renderer, fontId, marginLeft, marginTop);

  if (!words.empty()) {
    drawHighlightWithSnapshot();
  }

  drawHints();

  if (popup != Popup::None) {
    // The popup overdraws the page, so the snapshot no longer matches the
    // framebuffer — force the next render onto the full-repaint path.
    snapshotIdx = -1;
    // drawPopup overlays the framebuffer and refreshes the display itself.
    // I18N.get directly: tr() only accepts literal key names.
    GUI.drawPopup(renderer, I18N.get(popupMsg));
    return;
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
