#pragma once

#include <EpdFontFamily.h>
#include <GfxRenderer.h>

#include <cstdint>
#include <memory>

#include "Epub/Page.h"

// Per-word positional data extracted from a rendered page.
// Stored in a flat array grouped by line.
struct WordInfo {
  int16_t textOffset;  // Offset into string arena
  int16_t textLen;     // Length of word in arena
  int16_t screenX;     // Absolute X on screen
  int16_t screenY;     // Absolute Y on screen
  int16_t width;       // Measured text width
  int16_t height;      // Line height from font
  EpdFontFamily::Style style;
};

// Manages word-selection cursor state for the dictionary lookup feature.
// This is a mode within EpubReaderActivity, not a separate Activity.
class WordSelectionMode {
 public:
  static constexpr int MAX_WORDS = 300;
  static constexpr int MAX_LINES = 40;
  static constexpr int ARENA_SIZE = 2048;

  void extractWords(const Page& page, const GfxRenderer& renderer, int fontId, int marginLeft, int marginTop);
  void clear();

  bool hasWords() const { return wordCount > 0; }
  bool isActive() const { return active; }
  void enter();
  void exit();

  // Cursor navigation
  void moveUp();
  void moveDown();
  void moveLeft();
  void moveRight();

  // Get currently selected word text (null-terminated, stack buffer)
  // Returns false if no word selected.
  bool getSelectedWord(char* outBuf, int outSize) const;

  // Render highlight over selected word (call after normal page render)
  void renderHighlight(GfxRenderer& renderer, int fontId) const;

  // Hyphenated word sub-selection
  bool isHyphenated() const;
  bool isInSubSelection() const { return subSelectionActive; }
  void enterSubSelection();
  void exitSubSelection();
  bool subSelectLeft();   // Returns false if already at first component
  bool subSelectRight();  // Returns false if already at last component
  bool getSubSelectedWord(char* outBuf, int outSize) const;

 private:
  // Word data
  WordInfo words[MAX_WORDS] = {};
  int wordCount = 0;

  // String arena: all word text stored contiguously
  char arena[ARENA_SIZE] = {};
  int arenaUsed = 0;

  // Line index: lineStarts[i] = index of first word on line i
  int lineStarts[MAX_LINES] = {};
  int lineCount = 0;

  // Cursor state
  bool active = false;
  int cursorLine = 0;
  int cursorWord = 0;  // Index within current line

  // Sub-selection for hyphenated words
  bool subSelectionActive = false;
  int subSelectIndex = 0;  // Which component of the hyphenated word
  int subSelectCount = 0;  // Total components

  int getGlobalWordIndex() const;
  int getLineWordCount(int line) const;
};
