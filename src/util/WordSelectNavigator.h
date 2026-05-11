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

  // Append a null-terminated string to a text pool. Returns the offset.
  // Uses manual linear +256 growth to avoid std::string doubling.
  static uint16_t poolAppend(std::string& pool, const char* s, size_t len);

  // Process navigation input for the current screen orientation.
  // Returns true if the selection changed (caller should requestUpdate).
  // Does NOT consume Confirm or Back.
  bool handleNavigation(const MappedInputManager& input, const GfxRenderer& renderer);

  // Currently highlighted word. nullptr if the word list is empty.
  const WordInfo* getSelected() const;

  // Hyphenated continuation of the selected word (EPUB use only).
  // Returns nullptr when there is no continuation.
  const WordInfo* getContinuation() const;

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
  // pixels under the new highlight, then draw the new highlight. The caller pushes
  // the returned dirty rect to the panel via GfxRenderer::displayBufferRegion.
  //
  // Returns std::nullopt when the caller must fall back to a full repaint:
  //   - the new highlight is too large for HighlightSnapshot's buffer
  //   - the selection has a hyphenated continuation (multi-word fallback for now)
  //   - the navigator is in multi-select mode
  //   - the snapshot capture is rejected by the renderer (e.g. out of bounds)
  //
  // Mutates internal snapshot state, so this method is non-const.
  std::optional<Rect> renderHighlightDifferential(GfxRenderer& renderer, int lineHeight,
                                                  int prevWordIdx, int currWordIdx);

  // Pixel snapshot for one rectangular framebuffer region. Stack-resident buffer
  // (no heap). Used by the differential-repaint path (renderHighlightDifferential,
  // added in a follow-up task) to capture pixels under a highlight before it is
  // drawn, so a later cursor move can restore them and wipe the highlight without
  // re-rendering the page.
  //
  // Sizing: 4096 bytes covers a worst-case ~800 x 40 px region (e.g., a row-spanning
  // multi-select highlight in single-row mode). When the requested region exceeds
  // capacity, capture() returns false and sets requiresFallback() so the caller can
  // fall back to a full repaint instead.
  struct HighlightSnapshot {
    static constexpr size_t MAX_SNAPSHOT_BYTES = 4096;

    // Capture the framebuffer rectangle into the internal buffer.
    // Returns true on success; false if the region exceeds capacity, is empty,
    // is out of bounds, or the renderer rejects it (sets fallback_ in those cases).
    bool capture(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const GfxRenderer& renderer);

    // Paste the captured pixels back into the framebuffer at the original
    // coordinates. No-op if the snapshot is not valid.
    void restore(GfxRenderer& renderer) const;

    bool valid() const { return bytes_ > 0; }
    bool requiresFallback() const { return fallback_; }
    void clear() {
      bytes_ = 0;
      fallback_ = false;
    }

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
    bool fallback_ = false;
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
