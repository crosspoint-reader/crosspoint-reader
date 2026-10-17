#pragma once

// Ported from crosspoint-reader (PR #2583 by Uri Tauber, HTML layout #2836 by
// Uri Tauber). Adapted to this device: a pinned font, and dictionary switching.

#include <Epub/Page.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"

// The font dictionary definitions are rendered in -- fixed, and deliberately
// NOT the reader's font.
//
// A definition is the only text in the firmware that needs the phonetic
// alphabet, and the phonetic alphabet is not free: shipping it in all twenty
// Noto Sans faces cost 124 KB of flash. Pinning the viewer to one face means
// only that face has to carry it. Noto Sans also reads well for reference text
// and looks distinct from the book, which is the right signal for an overlay.
//
// The cost is that a reader using Extra Large text still gets a 14pt
// definition.
#define DICTIONARY_FONT_ID NOTOSANS_14_FONT_ID

// Paged viewer for one dictionary definition. HTML definitions are laid out
// through the EPUB chapter parser into styled Pages; anything else (plain text,
// or HTML too damaged or too large to lay out) is word-wrapped once on entry and
// each page renders spans of the original string, so no per-line copies are held.
class DictionaryDefinitionActivity final : public Activity {
 public:
  // `dictionaryName` is shown alongside the page counter and is empty when only
  // one dictionary is installed -- which is also when Confirm does nothing, so
  // the name and the ability to switch appear together or not at all.
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string headword,
                                        std::string definition, bool htmlDefinition = false,
                                        std::string dictionaryName = {})
      : Activity("DictionaryDefinition", renderer, mappedInput),
        headword(std::move(headword)),
        definition(std::move(definition)),
        htmlDefinition(htmlDefinition),
        dictionaryName(std::move(dictionaryName)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // One wrapped display line: a byte span of `definition`. Wrapping keeps lines
  // under the screen width, so uint16_t length is ample.
  struct Line {
    uint32_t start;
    uint16_t len;
  };

  // Usable body-text rectangle between the header and the button hints. Carries its origin as
  // well as its size because the tap-to-page split has to land on exactly the rectangle the text
  // was drawn in -- one statement of it, read by both.
  struct BodyArea {
    int x;
    int y;
    int width;
    int height;
  };

  BodyArea bodyArea() const;
  bool layoutHtmlPages();
  void wrapText();
  int measureSpan(int fontId, const char* text, size_t len) const;
  void drawBody(int fontId, int x, int startY) const;

  const std::string headword;
  // Not const: onEnter() normalizes embedded NULs (StarDict multi-type
  // separators) to newlines so C-string APIs see the whole text, and the
  // plain-text path converts it in place, in this same buffer.
  std::string definition;
  const bool htmlDefinition;
  // Empty when there is nothing to switch to.
  const std::string dictionaryName;
  bool canSwitchDictionary() const { return !dictionaryName.empty(); }
  // Styled path: reader-identical Pages laid out from the HTML definition.
  // Empty means the plain-text span path below is active.
  std::vector<std::unique_ptr<Page>> pages;
  std::vector<Line> lines;
  int currentPage = 0;
  int totalPages = 1;
  int linesPerPage = 1;
  ButtonNavigator buttonNavigator;
};
