#pragma once

#include <Epub/Page.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Paged viewer for one dictionary definition. HTML definitions are laid out
// through the EPUB chapter parser into styled Pages; anything else (plain
// text, or HTML too damaged to parse) is word-wrapped once on entry and each
// page renders spans of the original string, so no per-line copies are held.
// On the styled path, Confirm opens DictionaryWordSelectActivity over the
// current page (borrowing it, see that class) so any word in the definition
// can be looked up in turn. A pick does NOT stack a new view: word-select
// hands the result back (DictionaryLookupResult) and this activity swaps its
// own content in place via showDefinition(), so chasing any number of
// cross-references never grows the activity stack past this one screen --
// Back always returns directly to whatever opened the first definition, and
// only one definition's Pages are ever resident at a time. The plain-text
// fallback has no per-word layout data, so it stays view-only.
class DictionaryDefinitionActivity final : public Activity {
 public:
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string headword,
                                        std::string definition, std::string dictionaryFolder,
                                        bool htmlDefinition = false)
      : Activity("DictionaryDefinition", renderer, mappedInput),
        headword(std::move(headword)),
        definition(std::move(definition)),
        dictionaryFolder(std::move(dictionaryFolder)),
        htmlDefinition(htmlDefinition) {}

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

  // Usable body-text area between the header and the button hints.
  struct BodyArea {
    int width;
    int height;
  };

  // Top-left of the body text in screen coordinates (orientation-aware);
  // also the margins DictionaryWordSelectActivity needs to hit-test words on
  // the same Page this activity renders.
  struct BodyOrigin {
    int x;
    int y;
  };

  BodyArea bodyArea() const;
  BodyOrigin bodyOrigin() const;
  // Normalizes `definition`, lays it out (layoutHtmlPages() or wrapText()),
  // and resets the page counter. Shared by onEnter() and showDefinition().
  void loadDefinition();
  bool layoutHtmlPages();
  void wrapText();
  int measureSpan(int fontId, const char* text, size_t len) const;
  void drawBody(int fontId, int x, int startY) const;
  void openWordSelect();
  // Swaps in a cross-referenced word's definition in place of the current
  // one (same activity, same stack depth) -- see the class comment.
  void showDefinition(std::string newHeadword, std::string newDefinition, bool newHtmlDefinition);

  // Not const: showDefinition() swaps these in place for a chained lookup.
  std::string headword;
  std::string definition;
  // Dictionary a chained lookup (openWordSelect) should search -- the same
  // one this definition itself came from (a vocab book's assigned dictionary,
  // or the reader's global SETTINGS.dictionaryName), not necessarily whatever
  // the global setting currently points to.
  const std::string dictionaryFolder;
  bool htmlDefinition;
  // Styled path: reader-identical Pages laid out from the HTML definition.
  // Empty means the plain-text span path below is active.
  std::vector<std::unique_ptr<Page>> pages;
  std::vector<Line> lines;
  int currentPage = 0;
  int totalPages = 1;
  int linesPerPage = 1;
  ButtonNavigator buttonNavigator;
};
