// Host-side litmus for WordSelectNavigator — word navigation and
// hyphenated-pair smoothing.
//
// Tests the fix that makes a hyphenated word behave as one navigation stop
// in horizontal directions, while leaving row navigation free to land on
// either half.
//
// Build/run: test/word-select-nav/run.sh

#include <GfxRenderer.h>
#include <MappedInputManager.h>

#include <cstdio>
#include <cstring>

#include "util/TextPool.h"
#include "util/WordSelectNavigator.h"

// --------------------------------------------------------------------------
// Tiny test harness
// --------------------------------------------------------------------------
static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg)                                              \
  do {                                                                \
    ++g_checks;                                                       \
    if (!(cond)) {                                                    \
      ++g_failures;                                                   \
      std::printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
    }                                                                 \
  } while (0)

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

static uint16_t poolAppendString(std::string& pool, const char* text) {
  return TextPool::append(pool, text, std::strlen(text));
}

static WordSelectNavigator::WordInfo mkWord(const char* text, int16_t x, int16_t y, int16_t width, int row) {
  WordSelectNavigator::WordInfo w;
  w.screenX = x;
  w.screenY = y;
  w.width = width;
  w.row = row;
  w.continuationIndex = -1;
  w.continuationOf = -1;
  w.textLen = static_cast<uint16_t>(std::strlen(text));
  w.lookupLen = w.textLen;
  // textOffset / lookupOffset filled by caller
  return w;
}

// Row 0:  wordA(10)  wordB(60)  under-(200)
// Row 1:                        stand(200) wordD(260) wordE(310)
//
// Hyphenated pair: under- (first half, index 2) + stand (second half, index 3)
// load() starts the cursor on the middle row/word (row 1, word 1 -> wordD).
static WordSelectNavigator makeHyphenatedFixture() {
  std::string pool;

  WordSelectNavigator::WordInfo w0 = mkWord("wordA", 10, 0, 40, 0);
  w0.textOffset = poolAppendString(pool, "wordA");
  w0.lookupOffset = w0.textOffset;

  WordSelectNavigator::WordInfo w1 = mkWord("wordB", 60, 0, 35, 0);
  w1.textOffset = poolAppendString(pool, "wordB");
  w1.lookupOffset = w1.textOffset;

  WordSelectNavigator::WordInfo w2 = mkWord("under-", 200, 0, 50, 0);
  w2.textOffset = poolAppendString(pool, "under-");
  w2.lookupOffset = w2.textOffset;

  WordSelectNavigator::WordInfo w3 = mkWord("stand", 200, 20, 45, 1);
  w3.textOffset = poolAppendString(pool, "stand");
  w3.lookupOffset = w3.textOffset;

  WordSelectNavigator::WordInfo w4 = mkWord("wordD", 260, 20, 40, 1);
  w4.textOffset = poolAppendString(pool, "wordD");
  w4.lookupOffset = w4.textOffset;

  WordSelectNavigator::WordInfo w5 = mkWord("wordE", 310, 20, 40, 1);
  w5.textOffset = poolAppendString(pool, "wordE");
  w5.lookupOffset = w5.textOffset;

  std::vector<WordSelectNavigator::WordInfo> words = {w0, w1, w2, w3, w4, w5};
  std::vector<WordSelectNavigator::Row> rows;
  WordSelectNavigator::organizeIntoRows(words, rows);
  // Run the real merge logic (the activity's path) so continuation links AND the
  // merged, hyphen-stripped lookup text ("understand") match production exactly —
  // hand-setting continuationIndex/continuationOf here would leave getLookup()
  // identical to getDisplay() and mask hyphen-stripping bugs in phrase building.
  WordSelectNavigator::mergeHyphenatedPairs(words, rows, pool);

  WordSelectNavigator nav;
  nav.load(std::move(words), std::move(rows), std::move(pool));
  return nav;
}

// Starting from the fixture's initial cursor (wordD), navigate to targetWord
// using Left/Right presses.
static void navigateTo(WordSelectNavigator& nav, MappedInputManager& input, GfxRenderer& renderer,
                       const char* targetWord) {
  const char* current = nav.getDisplay(*nav.getSelected());
  if (std::strcmp(current, targetWord) == 0) return;

  // Determine direction based on flat index
  int targetFlat = -1;
  for (int i = 0; i < 6; i++) {
    const auto* w = nav.getWordAt(i);
    if (w && std::strcmp(nav.getDisplay(*w), targetWord) == 0) {
      targetFlat = i;
      break;
    }
  }
  int currentFlat = nav.getCurrentFlatIndex();

  MappedInputManager::Button dir =
      (targetFlat > currentFlat) ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;

  for (int guard = 0; guard < 10; guard++) {
    input.reset();
    input.setReleased(dir, true);
    nav.handleNavigation(input, renderer);
    const char* now = nav.getDisplay(*nav.getSelected());
    if (std::strcmp(now, targetWord) == 0) return;
  }
}

// --------------------------------------------------------------------------
// Tests
// --------------------------------------------------------------------------

static void testOrganizeIntoRows() {
  std::printf("testOrganizeIntoRows\n");

  std::string pool;
  std::vector<WordSelectNavigator::WordInfo> words;
  // y=0
  words.push_back(mkWord("a", 0, 0, 10, 0));
  words.back().textOffset = poolAppendString(pool, "a");
  words.back().lookupOffset = words.back().textOffset;
  words.push_back(mkWord("b", 20, 0, 10, 0));
  words.back().textOffset = poolAppendString(pool, "b");
  words.back().lookupOffset = words.back().textOffset;
  // y=2 (within 2px tolerance -> same row)
  words.push_back(mkWord("c", 40, 2, 10, 0));
  words.back().textOffset = poolAppendString(pool, "c");
  words.back().lookupOffset = words.back().textOffset;
  // y=10 (new row)
  words.push_back(mkWord("d", 0, 10, 10, 0));
  words.back().textOffset = poolAppendString(pool, "d");
  words.back().lookupOffset = words.back().textOffset;

  std::vector<WordSelectNavigator::Row> rows;
  WordSelectNavigator::organizeIntoRows(words, rows);

  CHECK(rows.size() == 2, "two rows created");
  CHECK(rows[0].wordIndices.size() == 3, "row 0 has three words");
  CHECK(rows[1].wordIndices.size() == 1, "row 1 has one word");
  CHECK(words[0].row == 0, "word 0 in row 0");
  CHECK(words[1].row == 0, "word 1 in row 0");
  CHECK(words[2].row == 0, "word 2 in row 0 (within tolerance)");
  CHECK(words[3].row == 1, "word 3 in row 1");
}

static void testHyphenatedNavBackward() {
  std::printf("testHyphenatedNavBackward\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  // Fixture starts on wordD (flat index 4).
  // Press Left -> lands on stand (index 0 in row 1), then snaps to under- (first half).
  input.reset();
  input.setReleased(MappedInputManager::Button::Left, true);
  bool changed = nav.handleNavigation(input, renderer);

  CHECK(changed, "selection changed");
  const WordSelectNavigator::WordInfo* sel = nav.getSelected();
  CHECK(sel != nullptr, "has selected word");
  if (sel) {
    CHECK(std::strcmp(nav.getDisplay(*sel), "under-") == 0, "cursor on first half 'under-'");
  }

  // Press Left again -> should move to wordB.
  input.reset();
  input.setReleased(MappedInputManager::Button::Left, true);
  changed = nav.handleNavigation(input, renderer);
  CHECK(changed, "selection changed again");
  sel = nav.getSelected();
  if (sel) {
    CHECK(std::strcmp(nav.getDisplay(*sel), "wordB") == 0, "cursor on 'wordB'");
  }
}

static void testHyphenatedNavForward() {
  std::printf("testHyphenatedNavForward\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  // Navigate to wordB first (start from wordD, go left past the hyphenated pair).
  navigateTo(nav, input, renderer, "wordB");
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "wordB") == 0, "arrived at 'wordB'");

  // Press Right -> land on under- (first half).
  input.reset();
  input.setReleased(MappedInputManager::Button::Right, true);
  nav.handleNavigation(input, renderer);

  const WordSelectNavigator::WordInfo* sel = nav.getSelected();
  CHECK(sel != nullptr, "has selected word");
  if (sel) {
    CHECK(std::strcmp(nav.getDisplay(*sel), "under-") == 0, "cursor on first half 'under-'");
  }

  // Press Right again -> row-wrap to stand, then skip past to wordD.
  input.reset();
  input.setReleased(MappedInputManager::Button::Right, true);
  bool changed = nav.handleNavigation(input, renderer);
  CHECK(changed, "selection changed");
  sel = nav.getSelected();
  if (sel) {
    CHECK(std::strcmp(nav.getDisplay(*sel), "wordD") == 0, "cursor on 'wordD' (skipped second half)");
  }
}

static void testHyphenatedNavRowNavExempt() {
  std::printf("testHyphenatedNavRowNavExempt\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  // Navigate to under- (first half, row 0) via row-nav (Up from wordD).
  // Using Up avoids the wordPrev→snap path, which would leave pendingSnapIdx
  // pointing at stand and cause the subsequent Down to mis-navigate.
  input.reset();
  input.setReleased(MappedInputManager::Button::Up, true);
  nav.handleNavigation(input, renderer);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "under-") == 0, "arrived at 'under-' via row-nav");

  // Press Down -> findClosestWord on row 1 picks stand (same X=200).
  // Row navigation should NOT snap back to under-; cursor stays on stand.
  input.reset();
  input.setReleased(MappedInputManager::Button::Down, true);
  bool changed = nav.handleNavigation(input, renderer);
  CHECK(changed, "selection changed");

  const WordSelectNavigator::WordInfo* sel = nav.getSelected();
  CHECK(sel != nullptr, "has selected word");
  if (sel) {
    CHECK(std::strcmp(nav.getDisplay(*sel), "stand") == 0,
          "row nav lands on second half 'stand', does NOT snap to first half");
  }
}

static void testHyphenatedGetPairedHalf() {
  std::printf("testHyphenatedGetPairedHalf\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  // Navigate to under- (first half).
  navigateTo(nav, input, renderer, "under-");
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "under-") == 0, "on first half");

  const WordSelectNavigator::WordInfo* sel = nav.getSelected();
  CHECK(sel != nullptr, "has selected word");
  if (sel) {
    const WordSelectNavigator::WordInfo* cont = nav.getPairedHalf();
    CHECK(cont != nullptr, "first half has continuation");
    if (cont) {
      CHECK(std::strcmp(nav.getDisplay(*cont), "stand") == 0, "continuation is 'stand'");
    }
  }
}

// Verify that pressing Right from the first half at the end of a row wraps
// to the second half and then skips it, even when the second half is the
// only word at its position in the row.
static void testForwardSkipAtRowBoundary() {
  std::printf("testForwardSkipAtRowBoundary\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  // Navigate to under- (first half, last word in row 0).
  navigateTo(nav, input, renderer, "under-");
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "under-") == 0, "cursor on 'under-'");

  // Press Right: wraps to row 1, would land on stand, but skip past to wordD.
  input.reset();
  input.setReleased(MappedInputManager::Button::Right, true);
  bool changed = nav.handleNavigation(input, renderer);
  CHECK(changed, "selection changed after row wrap");
  const WordSelectNavigator::WordInfo* sel = nav.getSelected();
  if (sel) {
    CHECK(std::strcmp(nav.getDisplay(*sel), "wordD") == 0, "after row-wrap forward skip, cursor on 'wordD'");
  }
}

// Single row: wordA(0) under-(1) stand(2)
// under- has continuationIndex=2, stand has continuationOf=1.
// load() centers on middle word = under- (wordInRow=1).
static WordSelectNavigator makeSingleRowHyphenatedFixture() {
  std::string pool;

  WordSelectNavigator::WordInfo w0 = mkWord("wordA", 10, 0, 40, 0);
  w0.textOffset = poolAppendString(pool, "wordA");
  w0.lookupOffset = w0.textOffset;

  WordSelectNavigator::WordInfo w1 = mkWord("under-", 60, 0, 50, 0);
  w1.textOffset = poolAppendString(pool, "under-");
  w1.lookupOffset = w1.textOffset;
  w1.continuationIndex = 2;

  WordSelectNavigator::WordInfo w2 = mkWord("stand", 120, 0, 45, 0);
  w2.textOffset = poolAppendString(pool, "stand");
  w2.lookupOffset = w2.textOffset;
  w2.continuationOf = 1;

  std::vector<WordSelectNavigator::WordInfo> words = {w0, w1, w2};
  std::vector<WordSelectNavigator::Row> rows;
  WordSelectNavigator::organizeIntoRows(words, rows);

  WordSelectNavigator nav;
  nav.load(std::move(words), std::move(rows), std::move(pool));
  return nav;
}

// Row 0:  wordA(10)  wordB(60)  under-(200)  ← first half, trailing hyphen
// Row 1:                        -stand(200)  wordD(260)  wordE(310)
//                                 ↑ second half, leading hyphen too
static WordSelectNavigator makeHyphenBothFixture() {
  std::string pool;

  WordSelectNavigator::WordInfo w0 = mkWord("wordA", 10, 0, 40, 0);
  w0.textOffset = poolAppendString(pool, "wordA");
  w0.lookupOffset = w0.textOffset;

  WordSelectNavigator::WordInfo w1 = mkWord("wordB", 60, 0, 35, 0);
  w1.textOffset = poolAppendString(pool, "wordB");
  w1.lookupOffset = w1.textOffset;

  WordSelectNavigator::WordInfo w2 = mkWord("under-", 200, 0, 50, 0);
  w2.textOffset = poolAppendString(pool, "under-");
  w2.lookupOffset = w2.textOffset;

  WordSelectNavigator::WordInfo w3 = mkWord("-stand", 200, 20, 45, 1);
  w3.textOffset = poolAppendString(pool, "-stand");
  w3.lookupOffset = w3.textOffset;

  WordSelectNavigator::WordInfo w4 = mkWord("wordD", 260, 20, 40, 1);
  w4.textOffset = poolAppendString(pool, "wordD");
  w4.lookupOffset = w4.textOffset;

  WordSelectNavigator::WordInfo w5 = mkWord("wordE", 310, 20, 40, 1);
  w5.textOffset = poolAppendString(pool, "wordE");
  w5.lookupOffset = w5.textOffset;

  std::vector<WordSelectNavigator::WordInfo> words = {w0, w1, w2, w3, w4, w5};
  std::vector<WordSelectNavigator::Row> rows;
  WordSelectNavigator::organizeIntoRows(words, rows);
  // See makeHyphenatedFixture: run the real merge so getLookup() reflects the
  // hyphen-stripped, merged text rather than mirroring getDisplay().
  WordSelectNavigator::mergeHyphenatedPairs(words, rows, pool);

  WordSelectNavigator nav;
  nav.load(std::move(words), std::move(rows), std::move(pool));
  return nav;
}

// When the only row ends with a hyphenated pair, pressing Right from the first
// half should wrap around to word 0 — not get stuck on the second half.
static void testSingleRowForwardSkipWraps() {
  std::printf("testSingleRowForwardSkipWraps\n");
  WordSelectNavigator nav = makeSingleRowHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  // load() places cursor on under- (wordInRow=1, middle of 3).
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "under-") == 0, "cursor starts on 'under-'");

  // Press Right: moves to stand, smoothing code tries to skip past stand,
  // single-row else branch wraps to wordInRow=0 (wordA).
  input.reset();
  input.setReleased(MappedInputManager::Button::Right, true);
  bool changed = nav.handleNavigation(input, renderer);
  CHECK(changed, "selection changed");
  const WordSelectNavigator::WordInfo* sel = nav.getSelected();
  if (sel) {
    CHECK(std::strcmp(nav.getDisplay(*sel), "wordA") == 0,
          "single-row wrap: cursor on 'wordA', not stuck on second half");
  }
}

// After a wordPrev snap (second half → first half, crossing a row boundary),
// pressing rowPrev should reference the second half's row as the nav base, not
// the first half's row the cursor now sits on.
//
// Fixture:  row 0 — wordA  wordB  under-
//           row 1 — stand  wordD  wordE
//
// Sequence: start at wordD (row 1) → Left → land on stand → snap to under- (row 0).
// Then Up:
//   Without fix: rowNavBase = currentRow = 0 → targetRow = rowCount-1 = 1 → wraps to stand. WRONG.
//   With fix:    rowNavBase = stand.row = 1 → targetRow = 0 → stays on under-. CORRECT.
static void testHyphenatedBackwardThenRowPrev() {
  std::printf("testHyphenatedBackwardThenRowPrev\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  // Fixture starts on wordD (row 1). Left: land on stand → snap to under- (row 0).
  input.reset();
  input.setReleased(MappedInputManager::Button::Left, true);
  bool changed = nav.handleNavigation(input, renderer);
  CHECK(changed, "Left changed selection");
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "under-") == 0, "snapped to first half 'under-'");

  // Up: should navigate to row 0 (one above stand's row 1), not wrap to row 1.
  input.reset();
  input.setReleased(MappedInputManager::Button::Up, true);
  changed = nav.handleNavigation(input, renderer);
  CHECK(changed, "Up registered as a navigation event");
  const WordSelectNavigator::WordInfo* sel = nav.getSelected();
  CHECK(sel != nullptr, "has selected word after Up");
  if (sel) {
    CHECK(std::strcmp(nav.getDisplay(*sel), "under-") == 0,
          "Up after snap stays on row 0 ('under-'), does NOT wrap to 'stand' on row 1");
  }
}

// When the cursor arrives on the second half via row navigation and the user
// presses Left, the pair should be treated as one stop: one Left skips past
// the first half and lands on the word before it.
//
// Fixture:  row 0 — wordA  wordB  under-
//           row 1 —               stand  wordD  wordE
//
// Sequence: navigate to under- (row 0) → Down → land on stand (second half,
//           row 1) → Left once → expect wordB, not under-.
static void testHyphenatedNavFromSecondHalfLeft() {
  std::printf("testHyphenatedNavFromSecondHalfLeft\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  // Navigate to under- via Up (row-nav) to avoid leaving pendingSnapIdx set.
  // If navigateTo were used, it would arrive via the wordPrev snap path and set
  // pendingSnapIdx=stand, causing the subsequent Down to wrap back to row 0.
  input.reset();
  input.setReleased(MappedInputManager::Button::Up, true);
  nav.handleNavigation(input, renderer);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "under-") == 0, "arrived at 'under-' via row-nav");

  // Down: row nav from row 0 → row 1, closest X to under-(200) is stand(200).
  input.reset();
  input.setReleased(MappedInputManager::Button::Down, true);
  nav.handleNavigation(input, renderer);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "stand") == 0, "row-nav landed on 'stand' (second half)");

  // Left once: should skip past the first half and land on wordB.
  input.reset();
  input.setReleased(MappedInputManager::Button::Left, true);
  bool changed = nav.handleNavigation(input, renderer);
  CHECK(changed, "selection changed on Left");
  const WordSelectNavigator::WordInfo* sel = nav.getSelected();
  if (sel) {
    CHECK(std::strcmp(nav.getDisplay(*sel), "wordB") == 0,
          "one Left from second half skips first half and lands on 'wordB'");
  }
}

// renderHighlight on a non-hyphenated word: exactly 1 fillRect + 1 drawText.
static void testRenderHighlightSingleWord() {
  std::printf("testRenderHighlightSingleWord\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  // Fixture starts on wordD (non-hyphenated).
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "wordD") == 0, "cursor on 'wordD'");

  renderer.resetCounters();
  nav.renderHighlight(renderer, 16);
  CHECK(renderer.fillRectCallCount == 1, "single word: 1 fillRect");
  CHECK(renderer.drawTextCallCount == 1, "single word: 1 drawText");
}

// renderHighlight on the first half of a hyphenated pair: 2 fillRect + 2 drawText.
static void testRenderHighlightHyphenatedBothHalves() {
  std::printf("testRenderHighlightHyphenatedBothHalves\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  navigateTo(nav, input, renderer, "under-");
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "under-") == 0, "cursor on 'under-'");

  renderer.resetCounters();
  nav.renderHighlight(renderer, 16);
  CHECK(renderer.fillRectCallCount == 2, "hyphenated first half: 2 fillRects (both halves)");
  CHECK(renderer.drawTextCallCount == 2, "hyphenated first half: 2 drawTexts (both halves)");
}

// renderHighlight when cursor is on the second half (via row-nav): 2 fillRect + 2 drawText.
static void testRenderHighlightHyphenatedFromSecondHalf() {
  std::printf("testRenderHighlightHyphenatedFromSecondHalf\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  // Row-nav Down from under- lands on stand (second half, same X).
  // Navigate to under- via Up (row-nav) to avoid leaving pendingSnapIdx set.
  input.reset();
  input.setReleased(MappedInputManager::Button::Up, true);
  nav.handleNavigation(input, renderer);
  input.reset();
  input.setReleased(MappedInputManager::Button::Down, true);
  nav.handleNavigation(input, renderer);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "stand") == 0, "cursor on 'stand' (second half)");

  renderer.resetCounters();
  nav.renderHighlight(renderer, 16);
  CHECK(renderer.fillRectCallCount == 2, "second half: 2 fillRects (both halves via continuationOf)");
  CHECK(renderer.drawTextCallCount == 2, "second half: 2 drawTexts (both halves via continuationOf)");
}

// renderHighlightDifferential returns nullopt: stub readFramebufferRegion returns 0
// (capture fails), and hyphenated words are always rejected by the fast path.
static void testRenderHighlightDifferentialFallback() {
  std::printf("testRenderHighlightDifferentialFallback\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  // Non-hyphenated word (wordD, flat index 4): capture fails because stub returns 0 bytes.
  navigateTo(nav, input, renderer, "wordD");
  const int wordDIdx = nav.getCurrentFlatIndex();
  auto result = nav.renderHighlightDifferential(renderer, 16, -1, wordDIdx);
  CHECK(!result.has_value(), "non-hyphenated: nullopt when readFramebufferRegion returns 0");

  // Hyphenated first half (under-, flat index 2): always nullopt (fast path rejected).
  navigateTo(nav, input, renderer, "under-");
  const int underIdx = nav.getCurrentFlatIndex();
  auto result2 = nav.renderHighlightDifferential(renderer, 16, -1, underIdx);
  CHECK(!result2.has_value(), "hyphenated word: nullopt (fast path not supported)");
}

// Multi-select highlight: continuation half outside [lo,hi] is still drawn.
// Anchor on wordB (index 1), cursor on under- (index 2, first half).
// The second half 'stand' (index 3) lies outside [1,2] and must also be drawn.
static void testRenderHighlightMultiSelectHyphenatedFirstHalf() {
  std::printf("testRenderHighlightMultiSelectHyphenatedFirstHalf\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  // Navigate to under- to position the cursor there.
  navigateTo(nav, input, renderer, "under-");
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "under-") == 0, "cursor on 'under-'");

  // Enter multi-select with anchor on under- (index 2), then manually simulate
  // an anchor one word earlier (wordB, index 1) by entering multi-select after
  // navigating back one step.
  navigateTo(nav, input, renderer, "wordB");
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "wordB") == 0, "cursor on 'wordB'");

  // Enter multi-select mode with anchor at wordB.
  input.reset();
  input.setPressed(MappedInputManager::Button::Confirm, true);
  input.setHeldTime(700);  // > 600ms default threshold
  std::string phrase;
  nav.handleMultiSelectInput(input, phrase);
  CHECK(nav.isMultiSelecting(), "entered multi-select mode");

  // Consume the release.
  input.reset();
  input.setReleased(MappedInputManager::Button::Confirm, true);
  nav.handleMultiSelectInput(input, phrase);

  // Move cursor to under- (first half, index 2).
  navigateTo(nav, input, renderer, "under-");
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "under-") == 0, "cursor on 'under-' in multi-select");
  CHECK(nav.isMultiSelecting(), "still in multi-select");

  // Range is wordB(1)..under-(2). 'stand'(3) is outside but is under-'s continuationIndex.
  // Expect 3 fillRects: wordB, under-, stand.
  renderer.resetCounters();
  nav.renderHighlight(renderer, 16);
  CHECK(renderer.fillRectCallCount == 3, "multi-select ending on first half: 3 fillRects (wordB, under-, stand)");
  CHECK(renderer.drawTextCallCount == 3, "multi-select ending on first half: 3 drawTexts");

  // Confirm the selection and verify the phrase includes the continuation half.
  input.reset();
  input.setReleased(MappedInputManager::Button::Confirm, true);
  std::string confirmedPhrase;
  auto action = nav.handleMultiSelectInput(input, confirmedPhrase);
  CHECK(action == WordSelectNavigator::MultiSelectAction::PhraseReady, "confirm yields PhraseReady");
  CHECK(confirmedPhrase == "wordB understand",
        "phrase uses merged hyphen-free lookup text even though the second half lay "
        "outside the flat-index range");
}

// Multi-select highlight: when the range starts on the second half (anchor on
// 'stand', index 3), the first half 'under-' (index 2) lies outside [3, hi]
// and must also be drawn.
static void testRenderHighlightMultiSelectHyphenatedSecondHalf() {
  std::printf("testRenderHighlightMultiSelectHyphenatedSecondHalf\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  CHECK(nav.getSelected() != nullptr && std::strcmp(nav.getDisplay(*nav.getSelected()), "wordD") == 0,
        "fixture starts on 'wordD'");

  // Row-nav Down from initial wordD position lands on stand (via row nav, exempt from snap).
  // Navigate to wordD first, then Up to row 0, then Down to row 1 to land on stand.
  input.reset();
  input.setReleased(MappedInputManager::Button::Up, true);
  nav.handleNavigation(input, renderer);
  input.reset();
  input.setReleased(MappedInputManager::Button::Down, true);
  nav.handleNavigation(input, renderer);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "stand") == 0, "cursor on 'stand' (second half)");

  // Enter multi-select with anchor on stand (index 3).
  input.reset();
  input.setPressed(MappedInputManager::Button::Confirm, true);
  input.setHeldTime(700);
  std::string phrase;
  nav.handleMultiSelectInput(input, phrase);
  CHECK(nav.isMultiSelecting(), "entered multi-select mode");

  // Consume the release.
  input.reset();
  input.setReleased(MappedInputManager::Button::Confirm, true);
  nav.handleMultiSelectInput(input, phrase);

  // Move cursor to wordD (index 4), so range is stand(3)..wordD(4).
  // 'under-'(2) is outside [3,4] but is stand's continuationOf; must be drawn.
  input.reset();
  input.setReleased(MappedInputManager::Button::Right, true);
  nav.handleNavigation(input, renderer);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "wordD") == 0, "cursor on 'wordD'");
  CHECK(nav.isMultiSelecting(), "still in multi-select");

  // Expect 3 fillRects: under-, stand, wordD.
  renderer.resetCounters();
  nav.renderHighlight(renderer, 16);
  CHECK(renderer.fillRectCallCount == 3, "multi-select starting on second half: 3 fillRects (under-, stand, wordD)");
  CHECK(renderer.drawTextCallCount == 3, "multi-select starting on second half: 3 drawTexts");

  // Confirm the selection and verify the phrase includes the first half of the pair.
  input.reset();
  input.setReleased(MappedInputManager::Button::Confirm, true);
  std::string confirmedPhrase;
  auto action = nav.handleMultiSelectInput(input, confirmedPhrase);
  CHECK(action == WordSelectNavigator::MultiSelectAction::PhraseReady, "confirm yields PhraseReady");
  CHECK(confirmedPhrase == "understand wordD",
        "phrase uses merged hyphen-free lookup text even though the first half lay "
        "outside the flat-index range");
}

// Multi-select phrase spanning both halves of a pair (anchor on 'wordA', cursor on
// 'wordE'): the merged lookup text must appear exactly once, not duplicated by
// emitting both 'under-' and 'stand' as separate lookup tokens.
static void testBuildPhraseHyphenatedPairNotDuplicated() {
  std::printf("testBuildPhraseHyphenatedPairNotDuplicated\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  navigateTo(nav, input, renderer, "wordA");
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "wordA") == 0, "cursor on 'wordA'");

  // Enter multi-select with anchor on wordA (index 0).
  input.reset();
  input.setPressed(MappedInputManager::Button::Confirm, true);
  input.setHeldTime(700);
  std::string phrase;
  nav.handleMultiSelectInput(input, phrase);
  CHECK(nav.isMultiSelecting(), "entered multi-select mode");

  // Consume the release.
  input.reset();
  input.setReleased(MappedInputManager::Button::Confirm, true);
  nav.handleMultiSelectInput(input, phrase);

  // Move cursor to wordE (index 5), so range is wordA(0)..wordE(5) and contains
  // both halves of the pair (under-=2, stand=3).
  navigateTo(nav, input, renderer, "wordE");
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "wordE") == 0, "cursor on 'wordE'");
  CHECK(nav.isMultiSelecting(), "still in multi-select");

  input.reset();
  input.setReleased(MappedInputManager::Button::Confirm, true);
  std::string confirmedPhrase;
  auto action = nav.handleMultiSelectInput(input, confirmedPhrase);
  CHECK(action == WordSelectNavigator::MultiSelectAction::PhraseReady, "confirm yields PhraseReady");
  CHECK(confirmedPhrase == "wordA wordB understand wordD wordE",
        "merged lookup text for the pair appears exactly once when both halves are "
        "inside the selected range");
}

// Run Tests A–E against any two-row fixture with the same layout as
// makeHyphenatedFixture. firstHalf / secondHalf are the display strings of
// the two pair members; the surrounding words are always wordA/wordB/wordD/wordE.
static void runHyphenNavSuite(const char* label, WordSelectNavigator (*make)(), const char* firstHalf,
                              const char* secondHalf) {
  std::printf("%s\n", label);

  // A: Left from wordD hits the second half, snaps to first half; second Left
  //    continues to wordB.
  {
    WordSelectNavigator nav = make();
    MappedInputManager input;
    GfxRenderer renderer;
    input.reset();
    input.setReleased(MappedInputManager::Button::Left, true);
    nav.handleNavigation(input, renderer);
    const WordSelectNavigator::WordInfo* sel = nav.getSelected();
    CHECK(sel && std::strcmp(nav.getDisplay(*sel), firstHalf) == 0, "A: snap to first half on Left");
    input.reset();
    input.setReleased(MappedInputManager::Button::Left, true);
    nav.handleNavigation(input, renderer);
    sel = nav.getSelected();
    CHECK(sel && std::strcmp(nav.getDisplay(*sel), "wordB") == 0, "A: second Left reaches wordB");
  }

  // B: Right from wordB lands on first half; next Right skips second half -> wordD.
  {
    WordSelectNavigator nav = make();
    MappedInputManager input;
    GfxRenderer renderer;
    navigateTo(nav, input, renderer, "wordB");
    input.reset();
    input.setReleased(MappedInputManager::Button::Right, true);
    nav.handleNavigation(input, renderer);
    const WordSelectNavigator::WordInfo* sel = nav.getSelected();
    CHECK(sel && std::strcmp(nav.getDisplay(*sel), firstHalf) == 0, "B: Right lands on first half");
    input.reset();
    input.setReleased(MappedInputManager::Button::Right, true);
    nav.handleNavigation(input, renderer);
    sel = nav.getSelected();
    CHECK(sel && std::strcmp(nav.getDisplay(*sel), "wordD") == 0, "B: second Right skips second half -> wordD");
  }

  // C: Up from wordD goes to first half; Down row-navigates to second half and
  //    stays there (no snap back to first half).
  {
    WordSelectNavigator nav = make();
    MappedInputManager input;
    GfxRenderer renderer;
    input.reset();
    input.setReleased(MappedInputManager::Button::Up, true);
    nav.handleNavigation(input, renderer);
    CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), firstHalf) == 0, "C: Up reaches first half");
    input.reset();
    input.setReleased(MappedInputManager::Button::Down, true);
    nav.handleNavigation(input, renderer);
    const WordSelectNavigator::WordInfo* sel = nav.getSelected();
    CHECK(sel && std::strcmp(nav.getDisplay(*sel), secondHalf) == 0, "C: Down lands on second half, not snapped away");
  }

  // D: after backward snap (Left from wordD -> first half), Up must stay on the
  //    first half's row, not jump above it.
  {
    WordSelectNavigator nav = make();
    MappedInputManager input;
    GfxRenderer renderer;
    input.reset();
    input.setReleased(MappedInputManager::Button::Left, true);
    nav.handleNavigation(input, renderer);
    CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), firstHalf) == 0, "D: snapped to first half");
    input.reset();
    input.setReleased(MappedInputManager::Button::Up, true);
    nav.handleNavigation(input, renderer);
    const WordSelectNavigator::WordInfo* sel = nav.getSelected();
    CHECK(sel && std::strcmp(nav.getDisplay(*sel), firstHalf) == 0, "D: Up after snap stays on first half's row");
  }

  // E: Left from second half (arrived via row nav) skips the first half entirely.
  {
    WordSelectNavigator nav = make();
    MappedInputManager input;
    GfxRenderer renderer;
    input.reset();
    input.setReleased(MappedInputManager::Button::Up, true);
    nav.handleNavigation(input, renderer);
    input.reset();
    input.setReleased(MappedInputManager::Button::Down, true);
    nav.handleNavigation(input, renderer);
    CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), secondHalf) == 0, "E: on second half via row-nav");
    input.reset();
    input.setReleased(MappedInputManager::Button::Left, true);
    nav.handleNavigation(input, renderer);
    const WordSelectNavigator::WordInfo* sel = nav.getSelected();
    CHECK(sel && std::strcmp(nav.getDisplay(*sel), "wordB") == 0,
          "E: one Left from second half skips first half -> wordB");
  }
}

// A word that both starts and ends with '-' (e.g. -re-) must not be treated as
// the first half of a line-break compound, even when it is the last word on its
// row. mergeHyphenatedPairs guards this with a lastWord[0] == '-' check.
//
// Row 0:  wordA(10)  wordB(60)  -re-(200)   ← ends with '-' but starts with '-'
// Row 1:                        Test(200)  wordD(260)
//
// The test calls mergeHyphenatedPairs (the same function the activity uses) and
// asserts the fields directly before loading the navigator, so removing the guard
// from mergeHyphenatedPairs will make this test fail.
static void testHyphenBothEndsNotPaired() {
  std::printf("testHyphenBothEndsNotPaired\n");

  std::string pool;
  WordSelectNavigator::WordInfo w0 = mkWord("wordA", 10, 0, 40, 0);
  w0.textOffset = poolAppendString(pool, "wordA");
  w0.lookupOffset = w0.textOffset;

  WordSelectNavigator::WordInfo w1 = mkWord("wordB", 60, 0, 35, 0);
  w1.textOffset = poolAppendString(pool, "wordB");
  w1.lookupOffset = w1.textOffset;

  WordSelectNavigator::WordInfo w2 = mkWord("-re-", 200, 0, 30, 0);
  w2.textOffset = poolAppendString(pool, "-re-");
  w2.lookupOffset = w2.textOffset;

  WordSelectNavigator::WordInfo w3 = mkWord("Test", 200, 20, 35, 1);
  w3.textOffset = poolAppendString(pool, "Test");
  w3.lookupOffset = w3.textOffset;

  WordSelectNavigator::WordInfo w4 = mkWord("wordD", 260, 20, 40, 1);
  w4.textOffset = poolAppendString(pool, "wordD");
  w4.lookupOffset = w4.textOffset;

  std::vector<WordSelectNavigator::WordInfo> words = {w0, w1, w2, w3, w4};
  std::vector<WordSelectNavigator::Row> rows;
  WordSelectNavigator::organizeIntoRows(words, rows);

  // Run the actual merge logic — this is what the activity calls.
  // Without the guard, -re- would be paired with Test here.
  WordSelectNavigator::mergeHyphenatedPairs(words, rows, pool);

  CHECK(words[2].continuationIndex == -1, "-re- not paired: continuationIndex must stay -1 after merge");
  CHECK(words[3].continuationOf == -1, "Test not paired: continuationOf must stay -1 after merge");

  WordSelectNavigator nav;
  nav.load(std::move(words), std::move(rows), std::move(pool));
  MappedInputManager input;
  GfxRenderer renderer;

  input.reset();
  input.setReleased(MappedInputManager::Button::Left, true);
  nav.handleNavigation(input, renderer);
  const WordSelectNavigator::WordInfo* sel = nav.getSelected();
  CHECK(sel && std::strcmp(nav.getDisplay(*sel), "Test") == 0, "-re- not paired: Left from wordD lands on Test");

  input.reset();
  input.setReleased(MappedInputManager::Button::Left, true);
  nav.handleNavigation(input, renderer);
  sel = nav.getSelected();
  CHECK(sel && std::strcmp(nav.getDisplay(*sel), "-re-") == 0, "-re- not paired: second Left lands on -re-");

  renderer.resetCounters();
  nav.renderHighlight(renderer, 16);
  CHECK(renderer.fillRectCallCount == 1, "-re- not paired: renderHighlight draws only 1 highlight");
}

// mergeHyphenatedPairs must strip the trailing '-' from the first half AND the
// leading '-' from the second half so the lookup text is hyphen-free.
// e.g. "under-" + "-stand" → lookup "understand", not "under-stand".
static void testMergeLookupBothHyphens() {
  std::printf("testMergeLookupBothHyphens\n");

  std::string pool;
  WordSelectNavigator::WordInfo w0 = mkWord("wordA", 10, 0, 40, 0);
  w0.textOffset = poolAppendString(pool, "wordA");
  w0.lookupOffset = w0.textOffset;

  WordSelectNavigator::WordInfo w1 = mkWord("under-", 60, 0, 50, 0);
  w1.textOffset = poolAppendString(pool, "under-");
  w1.lookupOffset = w1.textOffset;

  WordSelectNavigator::WordInfo w2 = mkWord("-stand", 60, 20, 45, 1);
  w2.textOffset = poolAppendString(pool, "-stand");
  w2.lookupOffset = w2.textOffset;

  WordSelectNavigator::WordInfo w3 = mkWord("wordD", 120, 20, 40, 1);
  w3.textOffset = poolAppendString(pool, "wordD");
  w3.lookupOffset = w3.textOffset;

  std::vector<WordSelectNavigator::WordInfo> words = {w0, w1, w2, w3};
  std::vector<WordSelectNavigator::Row> rows;
  WordSelectNavigator::organizeIntoRows(words, rows);
  WordSelectNavigator::mergeHyphenatedPairs(words, rows, pool);

  CHECK(words[1].continuationIndex == 2, "under- paired with -stand");
  CHECK(words[2].continuationOf == 1, "-stand paired with under-");
  CHECK(std::strcmp(pool.data() + words[1].lookupOffset, "understand") == 0,
        "first-half lookup is 'understand', not 'under-stand'");
  CHECK(std::strcmp(pool.data() + words[2].lookupOffset, "understand") == 0,
        "second-half lookup is 'understand', not 'under-stand'");
}

static void testHyphenEndOnly() {
  runHyphenNavSuite("testHyphenEndOnly (\"under-\" + \"stand\")", makeHyphenatedFixture, "under-", "stand");
}

static void testHyphenBoth() {
  runHyphenNavSuite("testHyphenBoth (\"under-\" + \"-stand\")", makeHyphenBothFixture, "under-", "-stand");
}

int main() {
  std::printf("=== WordSelectNavigator host litmus ===\n");
  testOrganizeIntoRows();
  testHyphenatedNavBackward();
  testHyphenatedNavForward();
  testHyphenatedNavRowNavExempt();
  testHyphenatedGetPairedHalf();
  testForwardSkipAtRowBoundary();
  testSingleRowForwardSkipWraps();
  testHyphenatedBackwardThenRowPrev();
  testHyphenatedNavFromSecondHalfLeft();
  testRenderHighlightSingleWord();
  testRenderHighlightHyphenatedBothHalves();
  testRenderHighlightHyphenatedFromSecondHalf();
  testRenderHighlightDifferentialFallback();
  testRenderHighlightMultiSelectHyphenatedFirstHalf();
  testRenderHighlightMultiSelectHyphenatedSecondHalf();
  testBuildPhraseHyphenatedPairNotDuplicated();
  testHyphenBothEndsNotPaired();
  testMergeLookupBothHyphens();
  testHyphenEndOnly();
  testHyphenBoth();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
