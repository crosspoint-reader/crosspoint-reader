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
    bool isIpa = false;
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
  // Centres the initial selection on the middle row.
  // When consumeInitialConfirm is true, the first Confirm release is ignored
  // (prevents the long-press that opened word selection from also triggering multi-select).
  void load(std::vector<WordInfo> words, std::vector<Row> rows, std::string textPool,
            bool consumeInitialConfirm = false);

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

  // Compute the union of the previous and current highlight bounding rectangles,
  // padded by 2 px on every side to cover renderHighlight's fillRect border.
  // When prevWordIdx is -1 (no previous highlight), returns just the current
  // highlight's padded bounds.
  Rect computeDirtyRect(int prevWordIdx, int currWordIdx, int lineHeight) const;

  // Differential repaint: restore pixels under the previous highlight, snapshot
  // pixels under the new highlight, then draw the new highlight. The caller
  // pushes via GfxRenderer::displayBuffer (full panel) — the savings come from
  // skipping page->render, not from a smaller push. The returned dirty rect is
  // unused by current callers; only the optional's .has_value() distinguishes
  // success from fallback.
  //
  // Returns std::nullopt when the caller must fall back to a full repaint:
  //   - the new highlight is too large for HighlightSnapshot's buffer
  //   - the selection has a hyphenated continuation (multi-word fallback for now)
  //   - the navigator is in multi-select mode
  //   - the snapshot capture is rejected by the renderer (e.g. out of bounds)
  //
  // Mutates internal snapshot state, so this method is non-const.
  std::optional<Rect> renderHighlightDifferential(GfxRenderer& renderer, int lineHeight, int prevWordIdx,
                                                  int currWordIdx);

  // Pixel snapshot for one rectangular framebuffer region. The pixel buffer is
  // inline (a fixed-size array member), so the snapshot does no heap allocation
  // — but it lives wherever its enclosing WordSelectNavigator lives, which in
  // practice is on the heap inside DictionaryWordSelectActivity /
  // DictionaryDefinitionActivity. Used by renderHighlightDifferential to
  // capture pixels under a highlight before it is drawn, so a later cursor move
  // can restore them and wipe the old highlight without re-rendering the page.
  //
  // Relationship to GfxRenderer::storeBwBuffer / restoreBwBuffer: that pair
  // captures the *entire* framebuffer to a heap-allocated buffer for grayscale
  // rendering. HighlightSnapshot is region-scoped (one word's bounding rect)
  // and stays out of the heap, so it can run on every cursor move without
  // fragmentation pressure.
  //
  // Safety: the renderer byte-aligns the captured rectangle along the
  // panel-memory x-axis, expanding the captured region by up to 7 pixels per
  // side along that axis. In Landscape orientations this is the screen
  // horizontal axis (extra pixels on the left/right of the highlight); in
  // Portrait orientations the memory x-axis is the screen vertical axis, so
  // the extra pixels sit above and below the highlight in screen space.
  // restore() pastes those back unchanged, which is correct only as long as
  // nothing else writes to the framebuffer between capture() and restore().
  // In word-select that holds: the page is drawn once on entry and the only
  // other framebuffer writer is the highlight itself. Adding a path that
  // draws over those neighboring pixels (e.g. an overlay layered above this
  // one) would invalidate that assumption.
  //
  // Sizing: 4096 bytes covers a worst-case ~800 x 40 px region (e.g., a single
  // wide word's bounding rect in landscape). When the requested region exceeds
  // capacity, capture() returns false and the caller falls back to a full
  // repaint instead.
  struct HighlightSnapshot {
    static constexpr size_t MAX_SNAPSHOT_BYTES = 4096;

    // Capture the framebuffer rectangle into the internal buffer.
    // Returns true on success; false if the region exceeds capacity, is empty,
    // is out of bounds, or the renderer rejects it.
    bool capture(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const GfxRenderer& renderer);

    // Paste the captured pixels back into the framebuffer at the original
    // coordinates. No-op if the snapshot is not valid.
    void restore(GfxRenderer& renderer) const;

    bool valid() const { return bytes_ > 0; }
    void clear() { bytes_ = 0; }

    uint16_t x() const { return x_; }
    uint16_t y() const { return y_; }
    uint16_t w() const { return w_; }
    uint16_t h() const { return h_; }

   private:
    uint16_t x_ = 0;
    uint16_t y_ = 0;
    uint16_t w_ = 0;
    uint16_t h_ = 0;
    size_t bytes_ = 0;
    uint8_t buf_[MAX_SNAPSHOT_BYTES] = {};
  };

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

  // Snapshot of pixels under the most recently drawn highlight. Used by
  // renderHighlightDifferential to restore the framebuffer before drawing the
  // next highlight, so a cursor move repaints only the affected regions.
  HighlightSnapshot snapshot_;

  // Single-word highlight draw. Used by both renderHighlight (for each word it
  // chooses to highlight) and renderHighlightDifferential.
  void drawSingleHighlight(const GfxRenderer& renderer, int lineHeight, int wordIndex) const;

  // Padded bounding rectangle for one word, matching renderHighlight's ±2 padding.
  // Returns Rect{0,0,0,0} when wordIndex is invalid.
  Rect boundsForWord(int wordIndex, int lineHeight) const;
};
