#include "DictionaryWordSelectActivity.h"

#include <Epub/blocks/TextBlock.h>
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
#include <variant>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "components/UITheme.h"

namespace {

constexpr unsigned long POPUP_DURATION_MS = 1500;

// A token is selectable when it has an ASCII alphanumeric or a non-ASCII
// codepoint outside U+2000-U+206F (dashes, bullets and other General
// Punctuation that appear as standalone tokens are not words).
bool isSelectableToken(const char* text, const size_t length) {
  const auto* p = reinterpret_cast<const uint8_t*>(text);
  for (size_t i = 0; i < length; i++) {
    if (p[i] < 0x80) {
      if (std::isalnum(p[i])) return true;
    } else if (p[i] == 0xE2 && i + 2 < length && (p[i + 1] == 0x80 || p[i + 1] == 0x81)) {
      i += 2;  // skip the 3-byte General Punctuation codepoint
    } else {
      return true;
    }
  }
  return false;
}

// Word bytes for the purpose of finding a dash that joins two words.
bool isWordByte(const uint8_t c) { return c >= 0x80 || std::isalnum(c) != 0; }

// Byte offset just past the next em or en dash (U+2014 / U+2013) that has a word on both sides,
// scanning from `from`; 0 when there is none.
//
// A dash written without spaces fuses two words into one layout token -- "swiftness-a",
// "truth-that" -- and the reader looking one of them up means one of them. This is the cut. The
// dash stays with the LEFT piece so the pieces tile the token exactly (no gap to position) and
// the dictionary's own edge-stripping takes it off the lookup.
// A token with no word content of its own that carries an em or en dash separates the words on
// either side of it exactly as a space does. Bionic reading produces these: it tokenizes
// "swiftness-a" into a word span, the dash, and another word span, so the dash arrives as its
// own token rather than inside one.
bool isDashSeparatorToken(const char* text, const size_t length) {
  if (isSelectableToken(text, length)) return false;
  const auto* b = reinterpret_cast<const uint8_t*>(text);
  for (size_t i = 0; i + 2 < length; i++) {
    if (b[i] == 0xE2 && b[i + 1] == 0x80 && (b[i + 2] == 0x93 || b[i + 2] == 0x94)) return true;
  }
  return false;
}

size_t nextDashSplit(const char* text, const size_t length, const size_t from) {
  const auto* b = reinterpret_cast<const uint8_t*>(text);
  for (size_t i = from; i + 3 < length; i++) {
    if (b[i] != 0xE2 || b[i + 1] != 0x80) continue;
    if (b[i + 2] != 0x93 && b[i + 2] != 0x94) continue;  // en dash, em dash
    if (i == 0 || !isWordByte(b[i - 1]) || !isWordByte(b[i + 3])) continue;
    return i + 3;
  }
  return 0;
}

// A word broken across two lines ends on an ASCII hyphen -- either the one layout inserted at
// the break, or the compound's own. A token that is nothing but a dash is not a broken word.
bool endsOnHyphen(const char* text, const size_t length) { return length >= 2 && text[length - 1] == '-'; }

// The other half of a broken word starts on a word character, never on punctuation.
bool startsOnWordByte(const char* text) {
  const auto first = static_cast<uint8_t>(text[0]);
  return first >= 0x80 || std::isalnum(first) != 0;
}

// Fed to the watchdog while the index pass streams a multi-megabyte .idx.
void indexBuildYield(void*) { vTaskDelay(1); }

}  // namespace

void DictionaryWordSelectActivity::onEnter() {
  Activity::onEnter();
  // No null check: a failed allocation just disables the differential fast path
  // (drawHighlightWithSnapshot skips the read), leaving the full repaint.
  snapshot = makeUniqueNoThrow<uint8_t[]>(SNAPSHOT_CAPACITY);
  requestUpdate();
}

// Collect the selectable words and their screen boxes. Called from render()
// after the font cache has been prewarmed for this page: the boxes come from
// TextBlock::wordBox, which measures the text, and measuring an SD-card font
// before the prewarm loads every glyph from the card one overflow slot at a time.
// Index of `geometry`'s (font, scale, line height) in the side table, adding it if new. The
// table has one or two entries on a normal page, so the linear scan never runs long; the 255th
// distinct style reuses the last slot rather than overflowing the one-byte index.
uint8_t DictionaryWordSelectActivity::internDrawStyle(const TextBlock::WordBox& geometry) {
  for (size_t i = 0; i < drawStyles.size(); i++) {
    if (drawStyles[i].fontId == geometry.fontId && drawStyles[i].scale == geometry.scale &&
        drawStyles[i].height == geometry.height) {
      return static_cast<uint8_t>(i);
    }
  }
  if (drawStyles.size() >= UINT8_MAX) return static_cast<uint8_t>(drawStyles.size() - 1);
  drawStyles.push_back({geometry.scale, geometry.fontId, geometry.height});
  return static_cast<uint8_t>(drawStyles.size() - 1);
}

size_t DictionaryWordSelectActivity::countPageTokens(size_t& wordEstimate) const {
  size_t tokens = 0;
  wordEstimate = 0;
  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto& block = static_cast<const PageLine*>(element.get())->getBlock();
    if (!block || !block->valid()) continue;
    const uint16_t count = block->wordCount();
    tokens += count;
    // One group per token that does not continue the one before it. Reading the continuation
    // bits costs a byte each -- no text scan, no measurement.
    for (uint16_t i = 0; i < count; i++) {
      if (i == 0 || !block->wordContinues(i)) wordEstimate++;
    }
  }
  return tokens;
}

void DictionaryWordSelectActivity::extractWords() {
  // Reserve once, up front, from a real count. A dense page runs to ~280 tokens and bionic
  // reading roughly doubles that, so a fixed guess is either far too small -- and then the
  // vector doubles repeatedly, each growth holding the old block alongside the new -- or far too
  // large. Cutting a token at an em dash adds a piece here and there, hence the small slack.
  size_t wordEstimate = 0;
  const size_t tokenCount = countPageTokens(wordEstimate);
  drawStyles.clear();
  drawStyles.reserve(4);
  fragments.clear();
  fragments.reserve(tokenCount + tokenCount / 16 + 4);
  words.clear();
  words.reserve(wordEstimate + wordEstimate / 16 + 4);
  rowCount = 0;

  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block || !block->valid()) continue;

    bool rowHasWords = false;
    bool openGroup = false;           // words.back() is a group still being extended
    bool openSelectable = false;      // ...and it holds at least one selectable token
    bool previousEmitted = false;     // the previous block word produced a fragment
    uint16_t fragmentSourceWord = 0;  // block word the piece being added came from

    // A group with nothing selectable in it -- a lone dash, a run of quotes -- is dropped.
    // Both it and its fragments are the tail of their vectors, so this is a pop, not a gap.
    auto closeGroup = [&] {
      if (!openGroup) return;
      if (openSelectable) {
        rowHasWords = true;
      } else {
        fragments.resize(words.back().firstFragment);
        words.pop_back();
      }
      openGroup = false;
      openSelectable = false;
    };

    // Adds one piece of a token. `mayGlue` is false for every piece after a dash cut: a dash
    // between two words is a word boundary even though layout kept the pair in one token.
    auto addFragment = [&](const char* text, const size_t length, const int x, const int width,
                           const TextBlock::WordBox& geometry, const bool mayGlue) {
      const bool glued = mayGlue && openGroup && previousEmitted && fragmentSourceWord > 0 &&
                         block->wordContinues(fragmentSourceWord) && words.back().fragmentCount < UINT8_MAX;
      if (!glued) closeGroup();

      Fragment fragment;
      fragment.x = static_cast<int16_t>(x);
      fragment.y = geometry.y;
      fragment.width = static_cast<int16_t>(width);
      fragment.text = text;
      fragment.length = static_cast<uint16_t>(length);
      fragment.style = geometry.style;
      fragment.drawStyle = internDrawStyle(geometry);
      fragments.push_back(fragment);
      previousEmitted = true;

      if (glued) {
        Word& word = words.back();
        word.fragmentCount++;
        word.width = static_cast<int16_t>(fragment.x + fragment.width - word.x);
      } else {
        Word word;
        word.firstFragment = static_cast<uint16_t>(fragments.size() - 1);
        word.fragmentCount = 1;
        word.hyphenFragment = NO_HYPHEN;
        word.row = rowCount;
        word.x = fragment.x;
        word.width = fragment.width;
        words.push_back(word);
        openGroup = true;
      }
      // Punctuation glued to a word rides along -- the dictionary strips a word's edges
      // anyway -- but it never makes a group selectable on its own.
      openSelectable = openSelectable || isSelectableToken(text, length);
    };

    for (uint16_t i = 0; i < block->wordCount(); i++) {
      fragmentSourceWord = i;
      const char* text = block->wordText(i);
      const size_t textLength = block->wordTextLen(i);
      const TextBlock::WordBox geometry =
          block->wordBox(renderer, i, fontId, line->xPos + marginLeft, line->yPos + marginTop);
      // A token with no ink of its own -- the visible space that holds a no-break group
      // together -- is not part of any word, and it separates the words on either side of it.
      if (geometry.width <= 0 || geometry.height <= 0) {
        previousEmitted = false;
        continue;
      }
      // A bare dash between two words is a boundary, not a piece of either of them.
      if (isDashSeparatorToken(text, textLength)) {
        previousEmitted = false;
        continue;
      }

      // The common case: one whole token, measured once by wordBox and used in place.
      // Cutting needs a NUL-terminated copy to measure, so a token too long for the scratch
      // buffer is left whole -- those are never the dash-joined pairs this is for.
      if (textLength >= WORD_TEXT_CAPACITY || nextDashSplit(text, textLength, 0) == 0) {
        addFragment(text, textLength, geometry.x, geometry.width, geometry, /*mayGlue=*/true);
        continue;
      }

      size_t partStart = 0;
      int partX = geometry.x;
      bool firstPart = true;
      while (partStart < textLength) {
        size_t partEnd = nextDashSplit(text, textLength, partStart);
        if (partEnd == 0) partEnd = textLength;
        const size_t partLength = partEnd - partStart;

        char part[WORD_TEXT_CAPACITY];
        memcpy(part, text + partStart, partLength);
        part[partLength] = '\0';
        const int partWidth = (geometry.scale == 1.0f)
                                  ? renderer.getTextWidth(geometry.fontId, part, geometry.style)
                                  : renderer.getTextWidthScaled(geometry.fontId, part, geometry.style, geometry.scale);
        if (partWidth > 0) {
          addFragment(text + partStart, partLength, partX, partWidth, geometry, firstPart);
          firstPart = false;
        }

        if (partEnd < textLength) {
          // Advance, not ink width: the next piece starts where this one's pen ends.
          const int advance = renderer.getTextAdvanceX(geometry.fontId, part, geometry.style);
          partX += (geometry.scale == 1.0f) ? advance : static_cast<int>(advance * geometry.scale + 0.5f);
        }
        partStart = partEnd;
      }
    }
    closeGroup();
    if (rowHasWords) rowCount++;
  }

  joinHyphenatedWords();

  wordsExtracted = true;
  // Start on the middle row's word nearest mid-screen instead of top-left: any
  // word on the page is then at most half a page of moves away.
  if (!words.empty()) {
    const int initial = closestInRow(rowCount / 2, renderer.getScreenWidth() / 2);
    if (initial >= 0) selected = initial;
  }
}

void DictionaryWordSelectActivity::joinHyphenatedWords() {
  for (size_t i = 0; i + 1 < words.size(); i++) {
    Word& prefix = words[i];
    const Word& suffix = words[i + 1];
    // Words are in reading order, so a suffix one row down is by construction the last word of
    // its line meeting the first word of the next.
    if (suffix.row != prefix.row + 1) continue;
    // The fragments have to be adjacent for the joined word to stay one contiguous run.
    if (suffix.firstFragment != prefix.firstFragment + prefix.fragmentCount) continue;
    if (prefix.fragmentCount + suffix.fragmentCount > UINT8_MAX) continue;
    const Fragment& tail = fragments[suffix.firstFragment - 1];
    if (!endsOnHyphen(tail.text, tail.length)) continue;
    if (!startsOnWordByte(fragments[suffix.firstFragment].text)) continue;

    prefix.hyphenFragment = static_cast<uint8_t>(prefix.fragmentCount - 1);
    prefix.fragmentCount = static_cast<uint8_t>(prefix.fragmentCount + suffix.fragmentCount);
    words.erase(words.begin() + static_cast<long>(i) + 1);
  }
}

const char* DictionaryWordSelectActivity::fragmentText(const Fragment& fragment, char* buf, const size_t capacity) {
  if (fragment.text[fragment.length] == '\0') return fragment.text;  // whole token: no copy needed
  const size_t take = std::min(static_cast<size_t>(fragment.length), capacity - 1);
  memcpy(buf, fragment.text, take);
  buf[take] = '\0';
  return buf;
}

size_t DictionaryWordSelectActivity::buildWordText(const int index, char* buf, const size_t capacity,
                                                   const bool dropJoinHyphen) const {
  if (capacity == 0) return 0;
  const Word& word = words[index];
  size_t length = 0;
  for (uint8_t f = 0; f < word.fragmentCount && length + 1 < capacity; f++) {
    const Fragment& piece = fragments[word.firstFragment + f];
    const char* source = piece.text;
    size_t take = piece.length;
    if (dropJoinHyphen && f == word.hyphenFragment && take > 0 && source[take - 1] == '-') take--;
    take = std::min(take, capacity - 1 - length);
    memcpy(buf + length, source, take);
    length += take;
  }
  buf[length] = '\0';
  return length;
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

int DictionaryWordSelectActivity::wordAt(const int px, const int py) const {
  for (int index = 0; index < static_cast<int>(words.size()); index++) {
    const Word& word = words[index];
    for (uint8_t f = 0; f < word.fragmentCount; f++) {
      const Fragment& piece = fragments[word.firstFragment + f];
      if (px < piece.x || px >= piece.x + piece.width) continue;
      if (py >= piece.y && py < piece.y + drawStyleOf(piece).height) return index;
    }
  }
  return -1;
}

DictionaryWordSelectActivity::WordTouch DictionaryWordSelectActivity::consumeWordTouch() {
  if (!mappedInput.hasTouch() || words.empty()) return WordTouch::None;

  int x = 0;
  int y = 0;
  int index = -1;
  const auto hit = [&](const int px, const int py) {
    index = wordAt(px, py);
    return index >= 0;
  };

  // Claimed but not acted on: moving the cursor mid-contact would defeat the two-step below.
  if (mappedInput.wasScreenTouchDown(x, y) && hit(x, y)) return WordTouch::Consumed;
  if (!mappedInput.wasScreenTapped(x, y) || !hit(x, y)) return WordTouch::None;

  switch (ListRowTap::apply(index, static_cast<int>(words.size()), selected)) {
    case ListRowTap::Result::Rejected:
      return WordTouch::Consumed;
    case ListRowTap::Result::Selected:
      return WordTouch::Moved;
    case ListRowTap::Result::Activate:
      return WordTouch::Activate;
  }
  return WordTouch::Consumed;
}

void DictionaryWordSelectActivity::moveVertical(const int direction) {
  const Word& current = words[selected];
  const int centerX = current.x + current.width / 2;
  // Rows can be empty: joining a hyphenated word pulls the only word off the row that carried
  // its tail. Keep going rather than letting one such row wall the cursor in.
  for (int row = static_cast<int>(current.row) + direction; row >= 0 && row < static_cast<int>(rowCount);
       row += direction) {
    const int best = closestInRow(static_cast<uint16_t>(row), centerX);
    if (best < 0) continue;
    if (best != selected) {
      selected = best;
      requestUpdate();
    }
    return;
  }
}

bool DictionaryWordSelectActivity::ensureDictionaryOpen() {
  if (dictOpenAttempted) return dictOpenOk;
  dictOpenAttempted = true;
  if (SETTINGS.dictionaryName[0] == '\0') return false;
  dictOpenOk = dict.open(SETTINGS.dictionaryName);
  // needsIndex() opens and validates the .qidx sidecar, so ask it once per open
  // rather than once per word: the answer only changes when we build the
  // sidecar ourselves, which only performLookup() does.
  dictNeedsIndex = dictOpenOk && dict.needsIndex();
  return dictOpenOk;
}

// Runs from loop() once the cursor has been still long enough, so the search is
// paid for while the user is still reading the page rather than after they
// commit. Deliberately does NOT build the index: that pass takes seconds on a
// large dictionary and belongs behind Confirm's popup, not in an idle tick.
void DictionaryWordSelectActivity::speculateForSelection() {
  if (speculationDisabled || popup != Popup::None || words.empty()) return;
  if (speculationIdx == selected) return;  // already resolved for this word
  if (millis() - lastMoveMs < SPECULATIVE_DEBOUNCE_MS) return;

  if (!ensureDictionaryOpen() || dictNeedsIndex) {
    speculationDisabled = true;
    return;
  }

  char text[WORD_TEXT_CAPACITY];
  buildWordText(selected, text, sizeof(text), true);
  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
  bool found = dict.resolve(text, speculationLocation, speculationHeadword, &result);
  // A word broken at a hyphen it already had ("US-Satellitensystems") is a real headword WITH
  // the hyphen, so the joined form gets a second chance before the word is called absent.
  if (!found && result == Dictionary::LookupResult::NotFound && isHyphenJoined(selected)) {
    buildWordText(selected, text, sizeof(text), false);
    found = dict.resolve(text, speculationLocation, speculationHeadword, &result);
  }
  speculationIdx = selected;
  if (found) {
    speculation = Speculation::Found;
  } else if (result == Dictionary::LookupResult::NotFound) {
    speculation = Speculation::Missing;
  } else {
    // The search did not reach a verdict (IO error). Say nothing rather than
    // marking a word absent, and let Confirm report the real failure.
    speculation = Speculation::Unknown;
  }
  // Only the highlight style changes, and only when the answer is Missing.
  if (speculation == Speculation::Missing) {
    speculationRepaintPending = true;
    requestUpdate();
  }
}

const std::vector<DictionaryEntry>& DictionaryWordSelectActivity::installedDictionaries() {
  if (!dictionariesDiscovered) {
    dictionariesDiscovered = true;
    DictionaryRegistry::discover(dictionaries);
  }
  return dictionaries;
}

void DictionaryWordSelectActivity::switchDictionary(const int direction) {
  const auto& list = installedDictionaries();
  if (list.size() < 2) return;

  int current = -1;
  for (size_t i = 0; i < list.size(); i++) {
    if (list[i].name == SETTINGS.dictionaryName) {
      current = static_cast<int>(i);
      break;
    }
  }
  // An unknown current entry (the setting names a folder no longer on the card)
  // starts the cycle at the beginning rather than refusing to move.
  const int count = static_cast<int>(list.size());
  const int next = (current < 0) ? 0 : ((current + direction) % count + count) % count;

  strncpy(SETTINGS.dictionaryName, list[next].name.c_str(), sizeof(SETTINGS.dictionaryName) - 1);
  SETTINGS.dictionaryName[sizeof(SETTINGS.dictionaryName) - 1] = '\0';
  SETTINGS.saveToFile();

  // Force the next lookup to open the new dictionary, and drop everything that
  // described the old one.
  dictOpenAttempted = false;
  dictOpenOk = false;
  dictNeedsIndex = false;
  dict.releaseCaches();
  speculation = Speculation::Unknown;
  speculationIdx = -1;
  speculationDisabled = false;

  performLookup();
}

void DictionaryWordSelectActivity::performLookup() {
  if (SETTINGS.dictionaryName[0] == '\0') {
    popup = Popup::Error;
    popupMsg = StrId::STR_DICT_NO_DICT_SET;
    popupTime = millis();
    requestUpdate();
    return;
  }

  popup = Popup::Busy;
  ensureDictionaryOpen();
  popupMsg = dictNeedsIndex ? StrId::STR_DICT_INDEXING : StrId::STR_DICT_LOOKING_UP;
  requestUpdateAndWait();  // paint the page + busy popup before blocking on SD

  bool ok = dictOpenOk;
  Dictionary::IndexResult indexResult = Dictionary::IndexResult::Ok;
  if (ok && dictNeedsIndex) {
    ok = dict.buildIndex(&indexBuildYield, nullptr, &indexResult);
    dictNeedsIndex = !ok;  // a successful build leaves the sidecar fresh; a failed one retries
  }

  std::string definition;
  std::string headword;
  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
  bool found = false;
  char text[WORD_TEXT_CAPACITY];
  buildWordText(selected, text, sizeof(text), true);
  if (ok && speculationIdx == selected && speculation != Speculation::Unknown) {
    // The index search already ran while the cursor sat here. A hit only has to
    // read the definition; a known miss needs no SD access at all.
    if (speculation == Speculation::Found) {
      headword = speculationHeadword;
      found = dict.readResolved(speculationLocation, definition, &result);
    } else {
      result = Dictionary::LookupResult::NotFound;
    }
  } else if (ok) {
    found = dict.lookup(text, definition, headword, &result);
    if (!found && result == Dictionary::LookupResult::NotFound && isHyphenJoined(selected)) {
      buildWordText(selected, text, sizeof(text), false);
      found = dict.lookup(text, definition, headword, &result);
    }
  }
  Dictionary::logLookup(SETTINGS.dictionaryName, text, ok ? Dictionary::resultName(result) : "open_or_index_failed",
                        found ? headword.c_str() : nullptr);

  if (found) {
    popup = Popup::None;
    const bool html = dict.definitionsAreHtml();
    // The definition viewer lays the text out and may take the styled path
    // through the chapter parser, which wants every contiguous byte it can get.
    // The session buffers are no use while it is on top, so hand them back.
    dict.releaseCaches();
    // Name the dictionary for the viewer only when there is another to switch
    // to; that is what turns its Confirm button on.
    const bool multiple = installedDictionaries().size() > 1;
    startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(
                               renderer, mappedInput, std::move(headword), std::move(definition), html,
                               multiple ? std::string(SETTINGS.dictionaryName) : std::string()),
                           [this](const ActivityResult& result) {
                             // The child overdrew the page; the snapshot no longer matches.
                             snapshotIdx = -1;
                             if (const auto* switchRequest = std::get_if<DictionarySwitchResult>(&result.data)) {
                               pendingDictionarySwitch = switchRequest->direction;
                             }
                             requestUpdate();
                           });
    return;
  }

  // Name the failure: a genuine miss is "Not found"; a word that WAS found but
  // couldn't be read is a real error -- and decompression, a low-memory
  // allocation and a generic read error are told apart.
  if (!ok) {
    popup = Popup::Error;
    // An index build allocates a scan buffer, so it fails the same way lookups
    // do on a fragmented heap -- name that rather than a generic error.
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

void DictionaryWordSelectActivity::loop() {
  // Acted on here rather than in the result handler that set it: that handler
  // runs while the activity stack is being popped, and starting another
  // activity from inside it would re-enter the manager mid-transition.
  if (pendingDictionarySwitch != 0) {
    const int direction = pendingDictionarySwitch;
    pendingDictionarySwitch = 0;
    switchDictionary(direction);
    return;
  }

  if (popup == Popup::NotFound || popup == Popup::Error) {
    if (millis() - popupTime >= POPUP_DURATION_MS) {
      popup = Popup::None;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !words.empty()) {
    performLookup();
    return;
  }

  if (words.empty()) return;

  const bool hasNextWord = selected + 1 < static_cast<int>(words.size());
  const int before = selected;

  // A tap moves the cursor onto the word under the finger; a second tap on the word it is
  // already on looks that word up. Point-then-confirm, as everywhere else, and it earns its keep
  // here more than most: a lookup searches the dictionary index off the SD card and opens a
  // whole activity, while a mis-tap on a page of small print costs one more tap.
  //
  // Resolved before the buttons and, when it moves the cursor, deliberately falls THROUGH to the
  // shared tail below -- the debounce restart and speculateForSelection() are the same for a
  // cursor moved by a finger as for one moved by a button.
  const WordTouch touch = consumeWordTouch();
  if (touch == WordTouch::Activate) {
    performLookup();
    return;
  }
  if (touch == WordTouch::Moved) {
    requestUpdate();
  } else if (touch == WordTouch::None) {
    // The cursor travels over the page as the reader sees it, so it follows the logical
    // directions: whichever button pair lies across the screen picks the neighbouring word, and
    // whichever runs up and down it changes line.
    if (mappedInput.wasLogicalPressed(MappedInputManager::Direction::Left) && selected > 0) {
      selected--;
      requestUpdate();
    } else if (mappedInput.wasLogicalPressed(MappedInputManager::Direction::Right) && hasNextWord) {
      selected++;
      requestUpdate();
    } else if (mappedInput.wasLogicalPressed(MappedInputManager::Direction::Up)) {
      moveVertical(-1);
    } else if (mappedInput.wasLogicalPressed(MappedInputManager::Direction::Down)) {
      moveVertical(1);
    }
  }
  // WordTouch::Consumed lands here having changed nothing, which is the point: a contact still
  // down claims the tap so nothing else reads it, and the tail is a no-op when the cursor has
  // not moved.
  //
  // Restart the debounce on every move, so holding a direction down resolves
  // once at the end instead of at every word passed through.
  if (selected != before) lastMoveMs = millis();

  speculateForSelection();
}

// Saves the pixels under words[selected]'s highlight box, then draws the
// highlight over them. Returns false when the pixels could not be saved (no
// buffer / oversize box) -- the highlight is drawn regardless, but the next
// cursor move must do a full repaint.
bool DictionaryWordSelectActivity::drawHighlightWithSnapshot() {
  const Word& word = words[selected];
  // Fragments up to and including the hyphenated one sit on the word's own row, the rest on
  // the row below; each run gets its own box, so nothing between the two lines is disturbed.
  const uint8_t runBounds[MAX_SNAPSHOT_RUNS + 1] = {
      0, word.hyphenFragment == NO_HYPHEN ? word.fragmentCount : static_cast<uint8_t>(word.hyphenFragment + 1),
      word.fragmentCount};

  // A word the speculative resolve has already proved absent gets an outline instead of the
  // inverse fill: the cursor is still obviously here, but the reader can see there is nothing
  // to look up without pressing Confirm and waiting for a "Not found" popup. Anything not yet
  // resolved keeps the plain highlight -- absence is only ever claimed on a verdict.
  const bool knownMissing = speculationIdx == selected && speculation == Speculation::Missing;

  snapshotRunCount = 0;
  size_t used = 0;
  bool saved = snapshot != nullptr;
  for (uint8_t run = 0; run < MAX_SNAPSHOT_RUNS; run++) {
    if (runBounds[run] >= runBounds[run + 1]) continue;
    const Fragment& firstPiece = fragments[word.firstFragment + runBounds[run]];
    int left = firstPiece.x;
    int top = firstPiece.y;
    int right = firstPiece.x + firstPiece.width;
    int bottom = firstPiece.y + drawStyleOf(firstPiece).height;
    for (uint8_t f = runBounds[run] + 1; f < runBounds[run + 1]; f++) {
      const Fragment& piece = fragments[word.firstFragment + f];
      left = std::min(left, static_cast<int>(piece.x));
      top = std::min(top, static_cast<int>(piece.y));
      right = std::max(right, piece.x + piece.width);
      bottom = std::max(bottom, piece.y + drawStyleOf(piece).height);
    }

    // The box is the run's exact advance box, grown by 2px so the highlight has
    // a little air around the glyphs. Clamped to the panel so save, draw and
    // restore all use the same rectangle.
    int hx = left - 2;
    int hy = top - 2;
    int hw = right - left + 4;
    int hh = bottom - top + 4;
    if (hx < 0) {
      hw += hx;
      hx = 0;
    }
    if (hy < 0) {
      hh += hy;
      hy = 0;
    }
    if (hx + hw > renderer.getScreenWidth()) hw = renderer.getScreenWidth() - hx;
    if (hy + hh > renderer.getScreenHeight()) hh = renderer.getScreenHeight() - hy;
    if (hw <= 0 || hh <= 0) continue;

    // Runs share the one buffer back to back; a run that no longer fits gives up the
    // differential path for the whole word rather than restoring half of it.
    size_t written = 0;
    if (saved) {
      // cppcheck-suppress arithOperationsOnVoidPointer ; get() is uint8_t*, cppcheck
      // mis-deduces the concept-constrained makeUniqueNoThrow<uint8_t[]> return type
      written = renderer.readFramebufferRegion(hx, hy, hw, hh, snapshot.get() + used, SNAPSHOT_CAPACITY - used);
      saved = written > 0;
    }
    if (saved) {
      snapshotRuns[snapshotRunCount] = {static_cast<int16_t>(hx), static_cast<int16_t>(hy), static_cast<int16_t>(hw),
                                        static_cast<int16_t>(hh), static_cast<uint16_t>(used)};
      snapshotRunCount++;
      used += written;
    }

    if (knownMissing) {
      renderer.drawRect(hx, hy, hw, hh, true);
    } else {
      renderer.fillRect(hx, hy, hw, hh, true);
    }
    for (uint8_t f = runBounds[run]; f < runBounds[run + 1]; f++) {
      const Fragment& piece = fragments[word.firstFragment + f];
      const DrawStyle& drawStyle = drawStyleOf(piece);
      char scratch[WORD_TEXT_CAPACITY];
      const char* text = fragmentText(piece, scratch, sizeof(scratch));
      if (drawStyle.scale == 1.0f) {
        renderer.drawText(drawStyle.fontId, piece.x, piece.y, text, knownMissing, piece.style);
      } else {
        renderer.drawTextScaled(drawStyle.fontId, piece.x, piece.y, text, knownMissing, piece.style, drawStyle.scale);
      }
    }
  }

  if (!saved) snapshotRunCount = 0;
  snapshotIdx = saved ? selected : -1;
  return saved;
}

// Front-button bar. Drawn last on every repaint path, including the
// differential highlight-only one, so it always ends up as the top layer even
// when a highlighted word's box falls under a hint's screen area. No
// side-button hints: the full-bleed reader page has no spare gutter for them,
// so a hint box there would hide text.
void DictionaryWordSelectActivity::drawHints() const {
  // No selectable word on this page: Confirm and navigation are all no-ops
  // (guarded by words.empty() in loop()), so only Back is hinted.
  if (words.empty()) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }
  // The cursor moves in all four directions; the front strip carries two of them and the side
  // buttons the other two, so the arrows have to be routed rather than fixed.
  const auto labels =
      mappedInput
          .mapHints(tr(STR_BACK), tr(STR_LOOKUP), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT), tr(STR_DIR_UP), tr(STR_DIR_DOWN))
          .front;
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void DictionaryWordSelectActivity::render(RenderLock&&) {
  // Differential fast path: only the highlight moved and the framebuffer still
  // holds a clean page (no popup or sub-activity since the last full repaint).
  // Restore the pixels under the old highlight, draw the new one, and push --
  // skipping the two-pass page render entirely.
  // `selected != snapshotIdx` is the cursor-moved case; the snapshot is also
  // stale when the cursor stayed put and only the speculation verdict changed,
  // which is what repaints a word as absent a moment after landing on it.
  const bool highlightMoved = !words.empty() && (selected != snapshotIdx || speculationRepaintPending);
  if (popup == Popup::None && snapshotIdx >= 0 && highlightMoved) {
    // displayBuffer() ends with a buffer swap, so the write framebuffer holds
    // the frame from TWO refreshes ago -- the reader menu the overlay was
    // opened from, or an older cursor position. Patching two word boxes into
    // that and re-displaying it ships the stale frame back to the panel, which
    // is why the menu reappeared and old highlights piled up. Resync to what is
    // actually on screen first; the saved pixels below were read from that same
    // frame, so the restore only lines up after this.
    renderer.syncWriteBufferFromDisplayed();
    for (uint8_t run = 0; run < snapshotRunCount; run++) {
      const SnapshotRun& saved = snapshotRuns[run];
      // cppcheck-suppress arithOperationsOnVoidPointer ; get() is uint8_t*, cppcheck
      // mis-deduces the concept-constrained makeUniqueNoThrow<uint8_t[]> return type
      renderer.writeFramebufferRegion(saved.x, saved.y, saved.width, saved.height, snapshot.get() + saved.offset);
    }
    // Batch-load just the highlighted word's glyphs before drawing them
    // white-on-black. clearCache() FIRST: prewarmCache() appends into the page
    // slots and deliberately never clears them -- that clear normally happens
    // once per page inside endScanAndPrewarm(), which this path skips. Without
    // it every cursor move took another ~4-5 KB slot and never gave it back,
    // walking the heap down from ~60 KB to ~14 KB in a dozen moves and making
    // the first lookup fail with "Low heap for N byte definition".
    //
    // Dropping the page's warmed glyphs costs this path nothing: it does not
    // re-render the page, only the two word boxes and the hints.
    auto* fcm = renderer.getFontCacheManager();
    fcm->clearCache();
    const Word& word = words[selected];
    for (uint8_t f = 0; f < word.fragmentCount; f++) {
      const Fragment& piece = fragments[word.firstFragment + f];
      char scratch[WORD_TEXT_CAPACITY];
      fcm->prewarmCache(drawStyleOf(piece).fontId, fragmentText(piece, scratch, sizeof(scratch)),
                        static_cast<uint8_t>(1u << (static_cast<uint8_t>(piece.style) & 0x03)));
    }
    speculationRepaintPending = false;
    if (drawHighlightWithSnapshot()) {
      drawHints();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return;
    }
    // Snapshot failed (oversize box) -- fall through to a full repaint.
  }

  renderer.clearScreen();

  // Same prewarm-scan-then-render pass the reader uses, so SD-card font glyphs
  // hit the in-RAM cache during the real draw -- and, on the first render, so
  // extractWords() measures against a warm cache instead of the card.
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->renderTextOnly(renderer, fontId, marginLeft, marginTop);
  scope.endScanAndPrewarm();
  if (!wordsExtracted) extractWords();
  page->render(renderer, fontId, marginLeft, marginTop);

  speculationRepaintPending = false;
  if (!words.empty()) drawHighlightWithSnapshot();
  drawHints();

  if (popup != Popup::None) {
    // The popup overdraws the page, so the snapshot no longer matches the
    // framebuffer -- force the next render onto the full-repaint path.
    snapshotIdx = -1;
    // drawPopup overlays the framebuffer and refreshes the display itself.
    // I18N.get directly: tr() only accepts literal key names.
    GUI.drawPopup(renderer, I18N.get(popupMsg));
    return;
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
