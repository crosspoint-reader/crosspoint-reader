#pragma once

#include <EpdFontFamily.h>

#include <climits>
#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;
class MappedInputManager;

// Orientation-aware word-selection navigator.
// Holds a flat list of on-screen words organised into rows and tracks the
// currently highlighted word.  Does NOT handle Confirm or Back — the calling
// activity owns those buttons.
class WordSelectNavigator {
 public:
  struct WordInfo {
    std::string text;
    std::string lookupText;
    int16_t screenX = 0;
    int16_t screenY = 0;
    int16_t width = 0;
    int16_t row = 0;
    int continuationIndex = -1;  // index of hyphenated second half (EPUB only)
    int continuationOf = -1;     // index of hyphenated first half (EPUB only)
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    WordInfo(const std::string& t, int16_t x, int16_t y, int16_t w, int16_t r,
             EpdFontFamily::Style s = EpdFontFamily::REGULAR)
        : text(t), lookupText(t), screenX(x), screenY(y), width(w), row(r), style(s) {}
  };

  struct Row {
    int16_t yPos = 0;
    std::vector<int> wordIndices;
  };

  // Load pre-populated, pre-organised words and rows.
  // Centres the initial selection on the middle row.
  void load(std::vector<WordInfo> words, std::vector<Row> rows);

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

  // Total word count.
  int getWordCount() const { return static_cast<int>(words.size()); }

  void reset();

 private:
  std::vector<WordInfo> words;
  std::vector<Row> rows;
  int currentRow = 0;
  int currentWordInRow = 0;

  int findClosestWord(int targetRow) const;
};
