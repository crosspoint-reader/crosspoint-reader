#pragma once

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
class DictionaryDefinitionActivity final : public Activity {
 public:
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string rawWord,
                                        std::string headword, std::string definition)
      : Activity("DictionaryDefinition", renderer, mappedInput),
        rawWord(std::move(rawWord)),
        headword(std::move(headword)),
        definition(std::move(definition)) {}

  void onEnter() override;
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
};
