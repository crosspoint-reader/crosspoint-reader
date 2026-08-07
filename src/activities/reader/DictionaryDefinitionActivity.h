#pragma once

#include <I18n.h>

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "util/Dictionary.h"
#include "util/DictionaryRegistry.h"

// Paged plain-text viewer for one dictionary definition. The definition is
// word-wrapped once on entry; each page renders spans of the original string,
// so no per-line copies are held.
//
// When more than one dictionary is installed, the front Left/Right buttons
// re-look-up the selected word in the previous/next installed dictionary (side Up/Down
// page through a multi-page definition instead — the two no longer share
// Left/Right the way most list activities do, since here both actions need
// to coexist on one screen).
//
// Confirm appends the headword and the definition on screen to the vocabulary
// list (VocabStore), so a word is saved after reading what it means rather
// than blind from the page.
class DictionaryDefinitionActivity final : public Activity {
 public:
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string rawWord,
                                        std::string headword, std::string definition, std::string sourceDictionary)
      : Activity("DictionaryDefinition", renderer, mappedInput),
        rawWord(std::move(rawWord)),
        headword(std::move(headword)),
        definition(std::move(definition)),
        sourceDictionary(std::move(sourceDictionary)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // One wrapped display line: a byte span of `definition`. Wrapping keeps
  // lines under the screen width, so uint16_t length is ample.
  struct Line {
    uint32_t start;
    uint16_t len;
  };

  void wrapText();
  int measureSpan(int fontId, const char* text, size_t len) const;
  // Body text font, resolved once in onEnter via
  // sdFontSystem.acquireDictionaryFont() (may load an SD font on demand;
  // released in onExit). 0 only before onEnter runs.
  int bodyFontId = 0;
  void drawBody(int fontId, int x, int startY) const;
  // Re-looks-up `word` in the dictionary `direction` steps away (wrapping),
  // replacing headword/definition and resetting to page 0. No-op with fewer
  // than two installed dictionaries.
  void switchDictionary(int direction);

  // Original selected word (pre-stemming): re-lookup must start from this,
  // not from `headword`, since a stemmed match in one dictionary may not
  // exist verbatim in another. Named rawWord, not word() — Arduino.h #defines
  // word(...) to makeWord(...).
  const std::string rawWord;
  // Not const: switchDictionary() replaces this with the matched headword
  // from a different dictionary (or `rawWord` itself on a miss).
  std::string headword;
  // Not const: onEnter() normalizes embedded NULs (StarDict multi-type
  // separators) to newlines so C-string APIs see the whole text.
  std::string definition;
  // Folder name of the dictionary the definition came from. Not necessarily
  // SETTINGS.dictionaryName: the word-select activity falls back to other
  // installed dictionaries when the selected one misses, and Left/Right
  // switching must start from the dictionary actually shown.
  const std::string sourceDictionary;
  std::vector<Line> lines;
  int currentPage = 0;
  int totalPages = 1;
  int linesPerPage = 1;
  ButtonNavigator buttonNavigator;

  // Dictionary-switching state. dictIndex is the position within
  // `dictionaries` matching the dictionary this definition came from; -1
  // (switching disabled) when it can't be resolved (e.g. the active
  // dictionary was removed from the SD card between lookup and now).
  std::vector<DictionaryEntry> dictionaries;
  int dictIndex = -1;
  // Reused across switches; each switchDictionary() call reopens it against
  // the newly selected folder.
  Dictionary dict;

  // Confirm-to-save state. `definitionShown` is false while `definition` holds
  // a status line ("Looking up...", "Not found") rather than real dictionary
  // text, which must never reach the vocabulary file. `savedCurrent` suppresses
  // the duplicate append from a second Confirm on the same definition; a
  // dictionary switch clears it, since that is a different entry.
  bool definitionShown = true;
  bool savedCurrent = false;
  // This activity is opened from a Confirm release in the word-select view, so
  // require a fresh press here before a release can save — otherwise a stale
  // edge from that same press files a word the user never asked for. Same
  // guard DictionaryWordSelectActivity uses on entry from the reader.
  bool confirmPressSeen = false;
  bool popupVisible = false;
  StrId popupMsg = StrId::STR_VOCAB_SAVED;
  unsigned long popupTime = 0;

  // Writes the current headword/definition to the vocabulary list and raises
  // the outcome popup.
  void saveToVocabulary();
};
