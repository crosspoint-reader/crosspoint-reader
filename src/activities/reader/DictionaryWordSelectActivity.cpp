#include "DictionaryWordSelectActivity.h"

#include <BidiUtils.h>
#include <Epub/Section.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "components/UITheme.h"
#include "util/DictionaryRegistry.h"
#include "util/HighlightStore.h"

namespace {

constexpr unsigned long POPUP_DURATION_MS = 1500;

// DictionaryHighlight mode: Confirm held at least this long looks the word
// up; a shorter press anchors/saves the highlight selection.
constexpr unsigned long DICT_LOOKUP_HOLD_MS = 400;

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
  buildReadingOrder();
  resetCursorToMiddle();
  requestUpdate();
}

// Start on the middle row's word nearest mid-screen instead of top-left:
// any word on the page is then at most half a page of moves away.
void DictionaryWordSelectActivity::resetCursorToMiddle() {
  selected = 0;
  if (words.empty()) return;
  const int initial = closestInRow(rowCount / 2, renderer.getScreenWidth() / 2);
  if (initial >= 0) selected = canonicalIndex(initial);
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
    const int ascender = renderer.getFontAscenderSize(fontId);
    const int rubyShift = block->getRubyShift(ascender);
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      const char* text = block->wordText(i);
      if (!isSelectableToken(text)) continue;

      WordBox box;
      box.x = static_cast<int16_t>(line->xPos + block->wordXpos(i) + marginLeft);
      box.y = static_cast<int16_t>(line->yPos + marginTop + rubyShift);
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

// Rebuilds the reading order of the page's words: rows top to bottom, RTL
// rows right to left. Needed because TextBlock keeps only the visual
// (left-to-right) order, so a highlight saved by visual index would come out
// reversed for Arabic/Hebrew text. Row direction is a majority vote of each
// word's first strong bidi class, so embedded LTR words (numbers, Latin
// names) inside an RTL row don't flip it.
void DictionaryWordSelectActivity::buildReadingOrder() {
  const size_t count = words.size();
  readingOrder.clear();
  readingOrder.reserve(count);
  readingPos.assign(count, 0);

  size_t rowStart = 0;
  while (rowStart < count) {
    size_t rowEnd = rowStart + 1;
    while (rowEnd < count && words[rowEnd].row == words[rowStart].row) rowEnd++;

    int rtlStrong = 0;
    int ltrStrong = 0;
    for (size_t i = rowStart; i < rowEnd; i++) {
      // fallback 0 answers "is the first strong char R/AL?"; fallback 1
      // answers "is it L?"; words with no strong char count for neither.
      if (BidiUtils::detectParagraphLevel(words[i].text, 0) == 1) {
        rtlStrong++;
      } else if (BidiUtils::detectParagraphLevel(words[i].text, 1) == 0) {
        ltrStrong++;
      }
    }

    if (rtlStrong > ltrStrong) {
      for (size_t i = rowEnd; i > rowStart; i--) readingOrder.push_back(static_cast<uint16_t>(i - 1));
    } else {
      for (size_t i = rowStart; i < rowEnd; i++) readingOrder.push_back(static_cast<uint16_t>(i));
    }
    rowStart = rowEnd;
  }

  for (size_t pos = 0; pos < readingOrder.size(); pos++) {
    readingPos[readingOrder[pos]] = static_cast<uint16_t>(pos);
  }

  detectHyphenJoins();
}

// Links words the layout engine split across lines back into one logical
// unit. The split happens at cache-generation time (ParsedText::
// hyphenateWordAtIndex) and TextBlock keeps no record of it, so this is a
// shape heuristic: a row's last reading-order word ending in '-' after a
// word character, followed by a word on the very next row. A genuine hyphen
// that merely landed on a line end also matches; performLookup falls back to
// the bare fragment, so a wrong join can over-extend the highlight but never
// lose a lookup. Splits can chain (a long word re-split on the following
// line), hence per-word links rather than pairs. Fragments separated by a
// page boundary stay unlinked — the other half is not on this page.
void DictionaryWordSelectActivity::detectHyphenJoins() {
  for (size_t p = 0; p + 1 < readingOrder.size(); p++) {
    const uint16_t a = readingOrder[p];
    const uint16_t b = readingOrder[p + 1];
    if (words[a].row + 1 != words[b].row) continue;
    const char* text = words[a].text;
    const size_t len = strlen(text);
    if (len < 2 || text[len - 1] != '-') continue;
    const auto beforeHyphen = static_cast<unsigned char>(text[len - 2]);
    // Excludes "--" (em dash typed as two hyphens) and punctuation runs.
    if (beforeHyphen < 0x80 && std::isalnum(beforeHyphen) == 0) continue;
    words[a].joinedSuffix = static_cast<int16_t>(b);
    words[b].joinedPrefix = static_cast<int16_t>(a);
  }
}

// First word of the hyphenation chain containing idx (idx itself when
// unsplit). The selection cursor always sits on this index, never on a
// continuation fragment.
int DictionaryWordSelectActivity::canonicalIndex(int idx) const {
  while (words[idx].joinedPrefix >= 0) idx = words[idx].joinedPrefix;
  return idx;
}

// Extends a selection-range end over the continuation fragments of its last
// word. A continuation always immediately follows its prefix in reading
// order (last word of one row, first of the next), so stepping the chain is
// stepping reading positions.
int DictionaryWordSelectActivity::selectionEndPos(int hi) const {
  for (int i = readingOrder[hi]; words[i].joinedSuffix >= 0; i = words[i].joinedSuffix) hi++;
  return hi;
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
  const int centerX = current.x + current.width / 2;
  // A hyphenation chain occupies its continuation rows too: when the nearest
  // word on the target row folds back onto the current selection, keep going.
  for (int targetRow = static_cast<int>(current.row) + direction;
       targetRow >= 0 && targetRow < static_cast<int>(rowCount); targetRow += direction) {
    int best = closestInRow(static_cast<uint16_t>(targetRow), centerX);
    if (best < 0) continue;
    best = canonicalIndex(best);
    if (best == selected) continue;
    selected = best;
    requestUpdate();
    return;
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
  Dictionary::IndexResult indexResult = Dictionary::IndexResult::Ok;
  if (ok && indexing) ok = dict.buildIndex(&indexBuildYield, nullptr, &indexResult);

  std::string definition;
  std::string headword;
  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
  // A word the layout hyphenated across lines is looked up whole. Each
  // fragment's trailing '-' is either layout-inserted ("Quadrat-"+"kilometer"
  // -> "Quadratkilometer") or the word's own compound hyphen that the line
  // happened to break at ("well-"+"known" -> "well-known"); TextBlock keeps
  // no record of which, so try both, then the bare fragment — a mistaken
  // join can never lose a lookup that worked before.
  const char* lookupWord = words[selected].text;
  std::string joinedStripped;
  std::string joinedKept;
  bool found = false;
  if (ok && words[selected].joinedSuffix >= 0) {
    for (int i = selected; i >= 0; i = words[i].joinedSuffix) {
      const char* fragment = words[i].text;
      size_t len = strlen(fragment);
      joinedKept.append(fragment, len);
      if (words[i].joinedSuffix >= 0) len--;  // detectHyphenJoins guarantees the trailing '-'
      joinedStripped.append(fragment, len);
    }
    if (dict.lookup(joinedStripped.c_str(), definition, headword, &result)) {
      found = true;
      lookupWord = joinedStripped.c_str();
    } else if (dict.lookup(joinedKept.c_str(), definition, headword, &result)) {
      found = true;
      lookupWord = joinedKept.c_str();
    }
  }
  if (ok && !found) found = dict.lookup(words[selected].text, definition, headword, &result);

  // Genuine miss in the selected dictionary: retry the word in each other
  // installed dictionary (registry order, starting after the selected one —
  // the same "next" the definition view's Left/Right switching uses) so the
  // panel still opens instead of a dead-end "Not found" popup. A fallback
  // dictionary that fails to open/index/read is skipped; if every fallback
  // misses too, the selected dictionary's NotFound verdict below stands.
  std::string sourceDictionary = SETTINGS.dictionaryName;
  if (ok && !found && result == Dictionary::LookupResult::NotFound) {
    std::vector<DictionaryEntry> installed;
    DictionaryRegistry::discover(installed);
    size_t startIdx = 0;
    for (size_t i = 0; i < installed.size(); i++) {
      if (installed[i].name == SETTINGS.dictionaryName) {
        startIdx = i + 1;
        break;
      }
    }
    for (size_t step = 0; !found && step < installed.size(); step++) {
      const DictionaryEntry& entry = installed[(startIdx + step) % installed.size()];
      if (entry.name == SETTINGS.dictionaryName) continue;
      // `dict` now leaves the selected dictionary; drop the open cache so the
      // next lookup reopens the user's selection.
      dictOpenAttempted = false;
      if (!dict.open(entry.name.c_str())) continue;
      if (dict.needsIndex()) {
        popupMsg = StrId::STR_DICT_INDEXING;
        requestUpdateAndWait();
        if (!dict.buildIndex(&indexBuildYield)) continue;
      }
      if (!joinedStripped.empty() && dict.lookup(joinedStripped.c_str(), definition, headword)) {
        found = true;
        lookupWord = joinedStripped.c_str();
      } else if (!joinedKept.empty() && dict.lookup(joinedKept.c_str(), definition, headword)) {
        found = true;
        lookupWord = joinedKept.c_str();
      } else if (dict.lookup(words[selected].text, definition, headword)) {
        found = true;
        lookupWord = words[selected].text;
      }
      if (found) sourceDictionary = entry.name;
    }
  }

  if (found) {
    popup = Popup::None;
    startActivityForResult(
        std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, lookupWord, std::move(headword),
                                                       std::move(definition), std::move(sourceDictionary)),
        [this](const ActivityResult&) { requestUpdate(); });
    return;
  }
  // Name the failure: a genuine miss is "Not found"; a word that WAS found but
  // couldn't be read is a real error — and we distinguish decompression from a
  // low-memory allocation from a generic read error.
  if (!ok) {
    popup = Popup::Error;
    // An index build allocates a scan buffer, so it fails the same way lookups
    // do on a fragmented heap — name that rather than a generic error.
    switch (indexResult) {
      case Dictionary::IndexResult::LowMemory:
        popupMsg = StrId::STR_DICT_LOW_MEMORY;
        break;
      case Dictionary::IndexResult::ReadError:
        popupMsg = StrId::STR_DICT_READ_FAILED;
        break;
      case Dictionary::IndexResult::Ok:
      default:
        popupMsg = StrId::STR_DICT_ERROR;  // dict.open() failed, not the index
        break;
    }
  } else {
    switch (result) {
      case Dictionary::LookupResult::Decompress:
        popup = Popup::Error;
        popupMsg = StrId::STR_DICT_DECOMPRESS_ERROR;
        break;
      case Dictionary::LookupResult::LowMemory:
        popup = Popup::Error;
        popupMsg = StrId::STR_DICT_LOW_MEMORY;
        break;
      case Dictionary::LookupResult::ReadError:
        popup = Popup::Error;
        popupMsg = StrId::STR_DICT_READ_FAILED;
        break;
      case Dictionary::LookupResult::NotFound:
      default:
        popup = Popup::NotFound;
        popupMsg = StrId::STR_DICT_NOT_FOUND;
        break;
    }
  }
  popupTime = millis();
  requestUpdate();
}

void DictionaryWordSelectActivity::handleConfirmRelease() {
  switch (mode) {
    case Mode::Dictionary:
      performLookup();
      break;
    case Mode::Highlight:
      toggleHighlight();
      break;
    case Mode::DictionaryHighlight:
      if (mappedInput.getHeldTime() >= DICT_LOOKUP_HOLD_MS) {
        performLookup();
      } else {
        toggleHighlight();
      }
      break;
  }
}

// True when the row's stored (visual) order runs opposite to reading order.
bool DictionaryWordSelectActivity::rowIsRtl(const uint16_t row) const {
  int first = -1;
  int last = -1;
  for (size_t i = 0; i < words.size(); i++) {
    if (words[i].row != row) continue;
    if (first < 0) first = static_cast<int>(i);
    last = static_cast<int>(i);
  }
  return first >= 0 && readingPos[first] > readingPos[last];
}

void DictionaryWordSelectActivity::resetCarried() {
  carriedText.clear();
  carriedText.shrink_to_fit();
  carriedLens.clear();
  firstPageSelStart = -1;
}

// Loads and displays another page of the section, rebuilding the word list.
// On failure (bad index, load error, page with nothing selectable) the
// current page stays and false is returned.
bool DictionaryWordSelectActivity::showPage(const int pageIndex) {
  if (!section || pageIndex < 0 || pageIndex >= static_cast<int>(section->pageCount)) return false;
  auto next = section->loadPage(pageIndex);
  if (!next) return false;
  auto prev = std::move(page);
  page = std::move(next);
  extractWords();
  buildReadingOrder();
  if (words.empty()) {
    // An image-only page would strand the selection; back out.
    page = std::move(prev);
    extractWords();
    buildReadingOrder();
    return false;
  }
  sectionPageIndex = pageIndex;
  snapshotIdx = -1;
  drawnLo = drawnHi = -1;
  requestUpdate();
  return true;
}

// Extends the anchored selection onto the next page: everything from the
// selection start to the end of the current page joins carriedText and the
// selection re-anchors at the top of the new page.
bool DictionaryWordSelectActivity::advancePage() {
  if (carriedLens.size() >= MAX_CARRIED_PAGES) return false;
  const int lo = std::min(readingPos[anchor], readingPos[selected]);
  const size_t lenBefore = carriedText.size();
  for (size_t p = lo; p < words.size(); p++) {
    const uint16_t idx = readingOrder[p];
    // No space inside a hyphenation chain: "Quadrat-" + "kilometer".
    const bool continuesPrev = p > static_cast<size_t>(lo) && words[readingOrder[p - 1]].joinedSuffix == idx;
    if (!carriedText.empty() && !continuesPrev) carriedText += ' ';
    carriedText += words[idx].text;
  }
  if (!showPage(sectionPageIndex + 1)) {
    carriedText.resize(lenBefore);
    return false;
  }
  if (carriedLens.empty()) firstPageSelStart = lo;
  carriedLens.push_back(lenBefore);
  anchor = readingOrder[0];
  selected = anchor;
  return true;
}

// Undoes one page advance: the current page's part of the selection is
// dropped and the previous page is shown again, selected from where the
// passage entered it through its end.
bool DictionaryWordSelectActivity::retreatPage() {
  if (carriedLens.empty()) return false;
  if (!showPage(sectionPageIndex - 1)) return false;
  carriedText.resize(carriedLens.back());
  carriedLens.pop_back();
  const int startPos =
      carriedLens.empty() ? std::min(std::max(firstPageSelStart, 0), static_cast<int>(words.size()) - 1) : 0;
  anchor = readingOrder[startPos];
  selected = canonicalIndex(readingOrder[words.size() - 1]);
  return true;
}

// Maps an on-screen direction to the button that physically lies that way.
// In portrait the front Left/Right pair is horizontal below the page and the
// side buttons are stacked vertically; in landscape the rotation puts the
// front pair in a vertical column and the side buttons on a horizontal edge,
// so the axes trade: side buttons step words, the front pair jumps rows.
// Within each axis the direction follows isNavDirectionSwapped() — the same
// convention mapLabels() uses to place hint labels — so the cursor always
// moves the way the drawn hints say it will.
bool DictionaryWordSelectActivity::wasPressedVisual(const VisualDir dir) const {
  using Button = MappedInputManager::Button;
  const auto orientation = renderer.getOrientation();
  const bool landscape =
      orientation == GfxRenderer::LandscapeClockwise || orientation == GfxRenderer::LandscapeCounterClockwise;
  const bool swapped = mappedInput.isNavDirectionSwapped();
  Button button = Button::Left;
  if (landscape) {
    // LandscapeCW: side buttons sit on the bottom edge (Down left of Up) and
    // the front pair on the left edge (Left above Right); CCW mirrors both.
    switch (dir) {
      case VisualDir::Left:
        button = swapped ? Button::Up : Button::Down;
        break;
      case VisualDir::Right:
        button = swapped ? Button::Down : Button::Up;
        break;
      case VisualDir::Up:
        button = swapped ? Button::Right : Button::Left;
        break;
      case VisualDir::Down:
        button = swapped ? Button::Left : Button::Right;
        break;
    }
  } else {
    // Portrait keeps the natural mapping; PortraitInverted reverses both
    // pairs (the device is upside down) when the follow-orientation swap is on.
    switch (dir) {
      case VisualDir::Left:
        button = swapped ? Button::Right : Button::Left;
        break;
      case VisualDir::Right:
        button = swapped ? Button::Left : Button::Right;
        break;
      case VisualDir::Up:
        button = swapped ? Button::Down : Button::Up;
        break;
      case VisualDir::Down:
        button = swapped ? Button::Up : Button::Down;
        break;
    }
  }
  return mappedInput.wasPressed(button);
}

// Cross-page selection: while anchored, a forward move past the last word
// (or visual-Down on the last row) continues the selection onto the next
// page; the symmetric backward move on the first word/row returns. True when
// the key press was consumed.
bool DictionaryWordSelectActivity::handleCrossPageNavigation() {
  if (anchor < 0 || !section || words.empty()) return false;
  const uint16_t row = words[selected].row;
  const bool rtl = rowIsRtl(row);
  const bool fwdKey = wasPressedVisual(rtl ? VisualDir::Left : VisualDir::Right);
  const bool backKey = wasPressedVisual(rtl ? VisualDir::Right : VisualDir::Left);
  const int pos = readingPos[selected];
  // The cursor sits on a chain's first fragment; the advance conditions test
  // the chain's far end (its continuation may be the page's last word / row).
  int endIdx = selected;
  int endPos = pos;
  while (words[endIdx].joinedSuffix >= 0) {
    endIdx = words[endIdx].joinedSuffix;
    endPos++;
  }
  if ((fwdKey && endPos == static_cast<int>(words.size()) - 1) ||
      (wasPressedVisual(VisualDir::Down) && words[endIdx].row == rowCount - 1)) {
    return advancePage();
  }
  if (!carriedLens.empty() && ((backKey && pos == 0) || (wasPressedVisual(VisualDir::Up) && row == 0))) {
    return retreatPage();
  }
  return false;
}

// First short press anchors a selection at the current word; the second saves
// the anchored range as a highlight and shows the outcome popup.
void DictionaryWordSelectActivity::toggleHighlight() {
  if (anchor < 0) {
    anchor = selected;
    resetCarried();
    // The cursor word is already highlighted; when the framebuffer is clean
    // (snapshot tracks it) seed the painted range from it so extending the
    // selection takes the incremental path without a repaint.
    if (snapshotIdx == selected) {
      // The snapshot path painted the whole hyphenation chain, so seed the
      // painted range over its continuations too.
      drawnLo = readingPos[selected];
      drawnHi = selectionEndPos(drawnLo);
    } else {
      drawnLo = drawnHi = -1;
      requestUpdate();
    }
    snapshotIdx = -1;  // the single-word snapshot is not maintained while selecting
    return;
  }
  const bool ok = saveHighlight();
  resetCarried();
  anchor = -1;
  drawnLo = drawnHi = -1;
  popup = ok ? Popup::Saved : Popup::Error;
  popupMsg = ok ? StrId::STR_HIGHLIGHT_SAVED : StrId::STR_HIGHLIGHT_SAVE_FAILED;
  popupTime = millis();
  requestUpdate();
}

// Joins the selected words in reading order (see buildReadingOrder), after
// any text carried over from previous pages, and appends the passage to the
// highlights markdown file.
bool DictionaryWordSelectActivity::saveHighlight() {
  const int lo = std::min(readingPos[anchor], readingPos[selected]);
  const int hi = selectionEndPos(std::max(readingPos[anchor], readingPos[selected]));
  size_t length = carriedText.size();
  for (int p = lo; p <= hi; p++) length += strlen(words[readingOrder[p]].text) + 1;
  std::string passage;
  passage.reserve(length);
  passage.append(carriedText);
  for (int p = lo; p <= hi; p++) {
    const uint16_t idx = readingOrder[p];
    // No space inside a hyphenation chain: "Quadrat-" + "kilometer".
    const bool continuesPrev = p > lo && words[readingOrder[p - 1]].joinedSuffix == idx;
    if (!passage.empty() && !continuesPrev) passage += ' ';
    passage += words[idx].text;
  }
  return HighlightStore::save(bookTitle, chapterTitle, passage);
}

void DictionaryWordSelectActivity::loop() {
  if (popup == Popup::NotFound || popup == Popup::Error || popup == Popup::Saved) {
    if (millis() - popupTime >= POPUP_DURATION_MS) {
      if (popup == Popup::Saved) {
        // Saving completes the task: return straight to the reader.
        finish();
        return;
      }
      popup = Popup::None;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmPressSeen = true;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (anchor >= 0) {
      // Cancel the in-progress selection; a full repaint clears its boxes.
      // A cross-page selection also returns to the page it started from.
      anchor = -1;
      drawnLo = drawnHi = -1;
      resetCarried();
      if (sectionPageIndex != originalPageIndex && showPage(originalPageIndex)) {
        resetCursorToMiddle();
      }
      requestUpdate();
    } else {
      finish();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && confirmPressSeen && !words.empty()) {
    handleConfirmRelease();
    return;
  }

  if (words.empty()) return;
  if (handleCrossPageNavigation()) return;
  // Step in reading order, not storage (visual) order. Within a row the two
  // only differ by which key means forward, but at a row boundary stepping
  // the storage index would jump from the last reading word of an RTL row to
  // the first reading word of the *previous* row instead of onto the next.
  const bool rtl = rowIsRtl(words[selected].row);
  const bool fwdKey = wasPressedVisual(rtl ? VisualDir::Left : VisualDir::Right);
  const bool backKey = wasPressedVisual(rtl ? VisualDir::Right : VisualDir::Left);
  const int pos = readingPos[selected];
  if (backKey && pos > 0) {
    // canonicalIndex folds a landing on a chain continuation onto its start.
    selected = canonicalIndex(readingOrder[pos - 1]);
    requestUpdate();
  } else if (fwdKey) {
    // Step over our own continuation fragments; the landing word is never a
    // continuation (its prefix would have to be the word before it).
    int next = pos + 1;
    while (next < static_cast<int>(words.size()) && words[readingOrder[next]].joinedPrefix >= 0) next++;
    if (next < static_cast<int>(words.size())) {
      selected = readingOrder[next];
      requestUpdate();
    }
  } else if (wasPressedVisual(VisualDir::Up)) {
    moveVertical(-1);
  } else if (wasPressedVisual(VisualDir::Down)) {
    moveVertical(1);
  }
}

// Saves the pixels under the selection's highlight, then draws it over
// them. The selection covers words[selected] plus its hyphenation
// continuations, so the saved region is the union box of the chain (which
// spans adjacent rows). Returns false when the pixels could not be saved
// (no buffer / oversize box, e.g. a long chain) — the highlight is drawn
// regardless, but the next cursor move must do a full repaint.
bool DictionaryWordSelectActivity::drawHighlightWithSnapshot() {
  int hx = INT_MAX;
  int hy = INT_MAX;
  int hxEnd = INT_MIN;
  int hyEnd = INT_MIN;
  for (int i = selected; i >= 0; i = words[i].joinedSuffix) {
    const WordBox& word = words[i];
    hx = std::min(hx, word.x - 2);
    hy = std::min(hy, word.y - 2);
    hxEnd = std::max(hxEnd, word.x + word.width + 2);
    hyEnd = std::max(hyEnd, word.y + lineHeight + 2);
  }
  // Clamp to the panel so save, draw and restore all use the same box.
  if (hx < 0) hx = 0;
  if (hy < 0) hy = 0;
  const int hw = hxEnd - hx;
  const int hh = hyEnd - hy;

  bool saved = false;
  if (snapshot && hw > 0 && hh > 0) {
    saved = renderer.readFramebufferRegion(hx, hy, hw, hh, snapshot.get(), SNAPSHOT_CAPACITY) > 0;
  }
  snapshotX = static_cast<int16_t>(hx);
  snapshotY = static_cast<int16_t>(hy);
  snapshotW = static_cast<int16_t>(hw);
  snapshotH = static_cast<int16_t>(hh);
  snapshotIdx = saved ? selected : -1;

  // Fill per fragment, not the union box: on a chain the union spans two
  // full rows and would black out unrelated words lying inside it.
  for (int i = selected; i >= 0; i = words[i].joinedSuffix) {
    const WordBox& word = words[i];
    const int bx = std::max(0, word.x - 2);
    const int by = std::max(0, word.y - 2);
    renderer.fillRect(bx, by, word.x + word.width + 2 - bx, word.y + lineHeight + 2 - by, true);
    renderer.drawText(fontId, word.x, word.y, word.text, false, word.style);
  }
  return saved;
}

// Paints (black box, white text) or clears (white box, black text) one word's
// highlight box directly, without snapshots. When a visually adjacent word on
// the same row also lies inside the [rangeLo, rangeHi] reading-position
// range, the box is stretched across the inter-word gap so the passage shows
// as one connected highlight (clearing passes the previously drawn range so
// stale bridges are erased too). Clearing can clip a few pixels of adjacent
// glyphs that overlap the padded box, which the next full repaint restores.
void DictionaryWordSelectActivity::paintWordBox(const int idx, const bool highlighted, const int rangeLo,
                                                const int rangeHi) {
  const WordBox& word = words[idx];
  int hx = word.x - 2;
  int hxEnd = word.x + word.width + 2;
  int hy = word.y - 2;
  int hh = lineHeight + 4;

  const auto inRange = [&](const int n) {
    return n >= 0 && n < static_cast<int>(words.size()) && words[n].row == word.row && readingPos[n] >= rangeLo &&
           readingPos[n] <= rangeHi;
  };
  // Words within a row are stored left to right, so idx-1/idx+1 are the
  // visual neighbours regardless of the text's reading direction.
  if (inRange(idx - 1)) hx = std::min(hx, static_cast<int>(words[idx - 1].x) + words[idx - 1].width + 2);
  if (inRange(idx + 1)) hxEnd = std::max(hxEnd, static_cast<int>(words[idx + 1].x) - 2);

  if (hx < 0) hx = 0;
  if (hy < 0) {
    hh += hy;
    hy = 0;
  }
  if (hxEnd <= hx || hh <= 0) return;
  renderer.fillRect(hx, hy, hxEnd - hx, hh, highlighted);
  // Outside a PrewarmScope the glyph cache may be empty; batch-load this word.
  renderer.getFontCacheManager()->prewarmCache(fontId, word.text,
                                               static_cast<uint8_t>(1u << (static_cast<uint8_t>(word.style) & 0x03)));
  renderer.drawText(fontId, word.x, word.y, word.text, !highlighted, word.style);
}

// Front-button bar (Back/Confirm/Left/Right). Drawn last on every repaint
// path, including the differential highlight-only paths, so it always ends
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
  // Confirm's meaning depends on the mode: lookup, highlight, or both
  // (the mixed mode hints the long-press lookup; a short press highlights).
  const char* confirmLabel = (mode == Mode::Highlight) ? tr(STR_HIGHLIGHT) : tr(STR_LOOKUP);
  // In landscape the front pair jumps rows instead of stepping words (see
  // wasPressedVisual), so hint the vertical directions; mapLabels' swap puts
  // each label on the chip whose button actually moves that way.
  const auto orientation = renderer.getOrientation();
  const bool landscape =
      orientation == GfxRenderer::LandscapeClockwise || orientation == GfxRenderer::LandscapeCounterClockwise;
  const char* prevLabel = landscape ? tr(STR_DIR_UP) : tr(STR_DIR_LEFT);
  const char* nextLabel = landscape ? tr(STR_DIR_DOWN) : tr(STR_DIR_RIGHT);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, prevLabel, nextLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void DictionaryWordSelectActivity::render(RenderLock&&) {
  // Incremental selection repaint: the framebuffer holds a clean page with
  // reading positions [drawnLo, drawnHi] highlighted; repaint only the words
  // entering or leaving the selection instead of re-running the two-pass
  // page render.
  if (popup == Popup::None && anchor >= 0 && drawnLo >= 0 && !words.empty()) {
    const int lo = std::min(readingPos[anchor], readingPos[selected]);
    // The range end is pulled over hyphenation continuations, so a selected
    // split word is always highlighted whole.
    const int hi = selectionEndPos(std::max(readingPos[anchor], readingPos[selected]));
    for (int p = drawnLo; p <= drawnHi; p++) {
      if (p < lo || p > hi) paintWordBox(readingOrder[p], false, drawnLo, drawnHi);
    }
    for (int p = lo; p <= hi; p++) {
      if (p < drawnLo || p > drawnHi) paintWordBox(readingOrder[p], true, lo, hi);
    }
    drawnLo = lo;
    drawnHi = hi;
    drawHints();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  // Differential fast path: only the highlight moved and the framebuffer
  // still holds a clean page (no popup or sub-activity since the last full
  // repaint). Restore the pixels under the old highlight, draw the new one,
  // and push — skipping the two-pass page render entirely.
  if (popup == Popup::None && anchor < 0 && snapshotIdx >= 0 && !words.empty() && selected != snapshotIdx) {
    renderer.writeFramebufferRegion(snapshotX, snapshotY, snapshotW, snapshotH, snapshot.get());
    // The full path's PrewarmScope cleared the glyph cache on exit; batch-load
    // just the highlighted words' glyphs (the whole hyphenation chain) before
    // drawing them white-on-black.
    for (int i = selected; i >= 0; i = words[i].joinedSuffix) {
      renderer.getFontCacheManager()->prewarmCache(
          fontId, words[i].text, static_cast<uint8_t>(1u << (static_cast<uint8_t>(words[i].style) & 0x03)));
    }
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
    if (anchor >= 0) {
      const int lo = std::min(readingPos[anchor], readingPos[selected]);
      const int hi = selectionEndPos(std::max(readingPos[anchor], readingPos[selected]));
      for (int p = lo; p <= hi; p++) paintWordBox(readingOrder[p], true, lo, hi);
      drawnLo = lo;
      drawnHi = hi;
      snapshotIdx = -1;
    } else {
      drawHighlightWithSnapshot();
      drawnLo = drawnHi = -1;
    }
  }

  drawHints();

  if (popup != Popup::None) {
    // The popup overdraws the page, so the snapshot no longer matches the
    // framebuffer — force the next render onto the full-repaint path.
    snapshotIdx = -1;
    drawnLo = drawnHi = -1;
    // drawPopup overlays the framebuffer and refreshes the display itself.
    // I18N.get directly: tr() only accepts literal key names.
    GUI.drawPopup(renderer, I18N.get(popupMsg));
    return;
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
