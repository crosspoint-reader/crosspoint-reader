#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "EpdFontFamily.h"
#include "activities/Activity.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"
#include "util/Dictionary.h"

// Paged plain-text viewer for one dictionary definition. The definition is
// word-wrapped once on entry; each page renders spans of the original string,
// so no per-line copies are held.
class DictionaryDefinitionActivity final : public Activity {
 public:
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string headword,
                                        std::string definition)
      : Activity("DictionaryDefinition", renderer, mappedInput),
        headword(std::move(headword)),
        definition(std::move(definition)),
        metadata(DictionaryPageMetadata(MetadataString{"", UI_12_FONT_ID, 0, 0, EpdFontFamily::REGULAR},
                                        MetadataString{"", UI_10_FONT_ID, 0, 0, EpdFontFamily::REGULAR})) {}

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
  void drawBody(int fontId, int x, int startY);
  void openDictionaryWordSelect();
  void saveWordsFromLine(const int fontId, const int startX, const int y, char* line, int& lineRow);
  void saveWordsFromPage();

  const std::string headword;
  // Not const: onEnter() normalizes embedded NULs (StarDict multi-type
  // separators) to newlines so C-string APIs see the whole text.
  DictionaryPageMetadata metadata;
  std::vector<WordBox> words;
  std::string definition;
  std::vector<Line> lines;
  int currentPage = 0;
  int totalPages = 1;
  int linesPerPage = 1;
  int nonBlankLineCount = 0;
  ButtonNavigator buttonNavigator;
  bool showDictionaryMessage = false;
  unsigned long dictionaryMessageTime = 0UL;
};
