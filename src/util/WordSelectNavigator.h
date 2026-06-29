#pragma once

#include <EpdFontFamily.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class GfxRenderer;
class MappedInputManager;

// Orientation-aware word-selection navigator.
// Holds a flat list of on-screen words organised into rows and tracks the
// currently highlighted word.  handleNavigation() processes directional input;
// handleMultiSelectInput() processes Confirm/Back for multi-word selection.
// The calling activity owns single-select Confirm/Back and activity-specific logic.
class WordSelectNavigator {
 public:
  struct WordInfo {
    uint16_t textOffset = 0;
    uint16_t textLen = 0;
    uint16_t lookupOffset = 0;
    uint16_t lookupLen = 0;
    int16_t screenX = 0;
    int16_t screenY = 0;
    int16_t width = 0;
    int16_t row = 0;
    int continuationIndex = -1;  // index of hyphenated second half (EPUB only)
    int continuationOf = -1;     // index of hyphenated first half (EPUB only)
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    bool isDictFont = false;
    int fontId = 0;  // resolved at extraction time; used by renderHighlight()
  };

  struct Row {
    int16_t yPos = 0;
    std::vector<int> wordIndices;
  };

  // Bounding rectangle in framebuffer coordinates. Used by the differential
  // repaint path to identify which screen region to push to the panel.
  struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
  };

  // Load pre-populated, pre-organised words, rows, and string pool.
  // Starts on the middle row and picks the word on that row whose center is
  // closest to initialPreferredX. The same X is then used as the initial
  // desired row-navigation position.
  // When consumeInitialConfirm is true, the first Confirm release is ignored
  // (prevents the long-press that opened word selection from also triggering multi-select).
  void load(std::vector<WordInfo> words, std::vector<Row> rows, std::string textPool, bool consumeInitialConfirm,
            int initialPreferredX);

  // Access null-terminated display text from the pool.
  const char* getDisplay(const WordInfo& w) const { return textPool.data() + w.textOffset; }
  // Access null-terminated lookup text from the pool.
  const char* getLookup(const WordInfo& w) const { return textPool.data() + w.lookupOffset; }

  // Organise a flat word list into rows by Y coordinate (2px tolerance).
  // Sets each word's row field and populates the rows vector.
  static void organizeIntoRows(std::vector<WordInfo>& words, std::vector<Row>& rows);

  // Link the last word of each row that ends with a trailing hyphen to the
  // first word of the next row, marking them as a compound pair via
  // continuationIndex / continuationOf. Also stores a merged lookup text
  // (hyphen stripped) shared by both halves for dictionary lookup.
  // Words whose text both starts and ends with '-' (e.g. -re-) are standalone
  // affix tokens and are skipped — they are not compound-word first halves.
  static void mergeHyphenatedPairs(std::vector<WordInfo>& words, const std::vector<Row>& rows, std::string& textPool);

  // Append a null-terminated string to a text pool. Returns the offset.
  // Uses manual linear +256 growth to avoid std::string doubling.
  static uint16_t poolAppend(std::string& pool, const char* s, size_t len);

  // Build a WordInfo from `display`/`lookup` text, append the text to `pool`,
  // and push it onto `words`. Centralises the pool-append + WordInfo-fill +
  // push_back boilerplate shared by the extraction routines in the Definition
  // and WordSelect activities.
  //
  // When `lookup` is nullptr the display text doubles as the lookup text and is
  // appended once (the EPUB word-select case, where the on-screen token is also
  // the lookup key). When `lookup` is non-null it is appended separately (the
  // definition case, where Dictionary::cleanWord produced a distinct key).
  // continuation* default to -1; hyphenation pairing is wired up afterwards by
  // mergeHyphenatedPairs.
  static void appendWord(std::vector<WordInfo>& words, std::string& pool, const char* display, size_t displayLen,
                         const char* lookup, size_t lookupLen, int16_t screenX, int16_t screenY, int16_t width,
                         EpdFontFamily::Style style, int fontId, bool isDictFont);

  // Process navigation input for the current screen orientation.
  // Returns true if the selection changed (caller should requestUpdate).
  // Does NOT consume Confirm or Back.
  bool handleNavigation(const MappedInputManager& input, const GfxRenderer& renderer);

  // Currently highlighted word. nullptr if the word list is empty.
  const WordInfo* getSelected() const;

  // The paired half of the selected hyphenated word (EPUB use only).
  // When on the first half returns the second half; when on the second half returns the first.
  // Returns nullptr when the selected word has no paired half.
  const WordInfo* getPairedHalf() const;

  bool isEmpty() const { return words.empty(); }

  // Flat index of the current cursor word. -1 if empty.
  int getCurrentFlatIndex() const;

  // Word at flat index idx. nullptr if out of bounds.
  const WordInfo* getWordAt(int idx) const;

  // Join display text of words in range [fromIdx, toIdx] (inclusive, either order).
  // Returns raw joined string; caller should apply Dictionary::cleanWord() if needed.
  std::string buildPhrase(int fromIdx, int toIdx) const;

  // --- Multi-select support (shared by WordSelect and Definition activities) ---

  enum class MultiSelectAction { None, Consumed, PhraseReady, ExitedMultiSelect, EnteredMultiSelect };

  bool isMultiSelecting() const { return inMultiSelectMode; }

  // Process Confirm/Back for multi-select state machine.
  // Returns PhraseReady when a phrase range is confirmed (raw phrase in outPhrase).
  // Returns EnteredMultiSelect on long-press Confirm that enters multi-select.
  // Returns Consumed when a long-press was detected but no valid word (caller should return).
  // Returns ExitedMultiSelect on Back during multi-select.
  // Returns None when no multi-select-relevant input occurred.
  MultiSelectAction handleMultiSelectInput(const MappedInputManager& input, std::string& outPhrase,
                                           unsigned long longPressMs = 600);

  // Draw inverted highlight for selected word(s).  Uses WordInfo::fontId.
  // In multi-select: highlights the anchor..cursor range.
  // In single-select: highlights the cursor word (+ hyphenated continuation if any).
  void renderHighlight(const GfxRenderer& renderer, int lineHeight) const;

  void reset();

 private:
  std::vector<WordInfo> words;
  std::vector<Row> rows;
  std::string textPool;
  int currentRow = 0;
  int currentWordInRow = 0;
  bool inMultiSelectMode = false;
  bool confirmReleaseConsumed = false;
  int anchorFlatIndex = -1;

  int findClosestWord(int targetRow) const;
  int findClosestWordFromX(int targetRow, int refCenterX) const;

  // Flat index of the second half we snapped from on wordPrev. Allows subsequent
  // rowPrev/rowNext to reference that half's position rather than the first half's.
  // -1 means inactive.
  int pendingSnapIdx = -1;

  // Preferred X position for consecutive row navigation. Preserves the user's
  // horizontal intent across rows even if an intermediate row can only land on
  // a far-left or far-right word (for example, the last word of a paragraph).
  // Cleared by horizontal navigation.
  int preferredRowNavX = -1;

  // Single-word highlight draw. Used by renderHighlight.
  void drawSingleHighlight(const GfxRenderer& renderer, int lineHeight, int wordIndex) const;

  // Draw the hyphenated continuation partner(s) of w when they fall outside [lo, hi].
  // No-op when w is nullptr or w has no continuation links.
  void drawContinuationsIfOutside(const GfxRenderer& renderer, int lineHeight, const WordInfo* w, int lo, int hi) const;
};
