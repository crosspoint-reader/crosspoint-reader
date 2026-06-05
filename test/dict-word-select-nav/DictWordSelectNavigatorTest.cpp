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
// Row 1:  stand(200) wordD(260) wordE(310)
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
  w2.continuationIndex = 3;  // points to second half

  WordSelectNavigator::WordInfo w3 = mkWord("stand", 200, 20, 45, 1);
  w3.textOffset = poolAppendString(pool, "stand");
  w3.lookupOffset = w3.textOffset;
  w3.continuationOf = 2;  // points back to first half

  WordSelectNavigator::WordInfo w4 = mkWord("wordD", 260, 20, 40, 1);
  w4.textOffset = poolAppendString(pool, "wordD");
  w4.lookupOffset = w4.textOffset;

  WordSelectNavigator::WordInfo w5 = mkWord("wordE", 310, 20, 40, 1);
  w5.textOffset = poolAppendString(pool, "wordE");
  w5.lookupOffset = w5.textOffset;

  std::vector<WordSelectNavigator::WordInfo> words = {w0, w1, w2, w3, w4, w5};
  std::vector<WordSelectNavigator::Row> rows;
  WordSelectNavigator::organizeIntoRows(words, rows);

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

  // Navigate to under- (first half, row 0).
  navigateTo(nav, input, renderer, "under-");
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "under-") == 0, "arrived at 'under-'");

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

static void testHyphenatedGetContinuation() {
  std::printf("testHyphenatedGetContinuation\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  // Navigate to under- (first half).
  navigateTo(nav, input, renderer, "under-");
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "under-") == 0, "on first half");

  const WordSelectNavigator::WordInfo* sel = nav.getSelected();
  CHECK(sel != nullptr, "has selected word");
  if (sel) {
    const WordSelectNavigator::WordInfo* cont = nav.getContinuation();
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

int main() {
  std::printf("=== WordSelectNavigator host litmus ===\n");
  testOrganizeIntoRows();
  testHyphenatedNavBackward();
  testHyphenatedNavForward();
  testHyphenatedNavRowNavExempt();
  testHyphenatedGetContinuation();
  testForwardSkipAtRowBoundary();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
