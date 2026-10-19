#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ClipSelectionActivity.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "util/Dictionary.h"

// Paged viewer for one dictionary definition. HTML definitions are laid out
// through the EPUB chapter parser into styled Pages; anything else (plain
// text, or HTML too damaged to parse) is word-wrapped once on entry and each
// page renders spans of the original string, so no per-line copies are held.
class DictionaryDefinitionActivity final : public Activity {
 public:
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string headword,
                                        std::string definition, bool htmlDefinition = false)
      : Activity("DictionaryDefinition", renderer, mappedInput),
        content{.headword = std::move(headword), .text = std::move(definition)},
        htmlDefinition(htmlDefinition) {}

  bool enableLookup(Dictionary& dictionary, DictLocation location);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // One wrapped display line: a byte span of `definition`. Wrapping keeps
  // lines under the screen width, so uint16_t length is ample.
  struct PlainSelectionLine;

  struct Line {
    uint32_t start;
    uint16_t len;
    bool bold;
  };

  // Usable body-text area between the header and the button hints.
  struct BodyArea {
    int x;
    int y;
    int width;
    int height;
  };

  BodyArea bodyArea() const;

  struct Definition {
    std::string headword;
    std::string text;
    std::vector<std::unique_ptr<Page>> pages;
    std::vector<Line> lines;
    int currentPage = 0;
    int totalPages = 1;
    int linesPerPage = 1;
    int selectionPage = 0;
    ClipSelectionActivity::Selection selection;

    void releasePayload() {
      std::string{}.swap(text);
      std::vector<std::unique_ptr<Page>>{}.swap(pages);
      std::vector<Line>{}.swap(lines);
    }
  };

  struct HistoryEntry {
    std::string headword;
    DictLocation location;
    int page = 0;
    int selectionPage = 0;
    ClipSelectionActivity::Selection selection;
    bool styled = false;
  };

  struct LookupState {
    explicit LookupState(Dictionary& dictionary, DictLocation location) : dictionary(dictionary), location(location) {}
    Dictionary& dictionary;  // Owned by the retained DictionaryWordSelectActivity parent.
    DictLocation location;
    std::array<HistoryEntry, 16> history;
    size_t depth = 0;
    std::string pendingWord;
    std::vector<std::unique_ptr<Page>> selectionPages;
  };

  bool prepareDefinition(Definition& next, bool forcePlain = false);
  bool layoutHtmlPages(Definition& next);
  bool wrapText(Definition& next);
  bool reserveLines(Definition& next, size_t count);
  int measureSpan(int fontId, const char* text, size_t len, EpdFontFamily::Style family = EpdFontFamily::REGULAR) const;
  void drawBody(int fontId, int x, int startY, int currentPage) const;
  void startSelection(int x = -1, int y = -1);
  bool buildPlainSelectionPages();
  void navigateDefinition(bool back);
  bool restoreCurrentDefinition(bool styled);
  void showMessage(StrId message);

  Definition content;
  const bool htmlDefinition;
  std::unique_ptr<LookupState> lookup;
  StrId message = StrId::STR_DICT_NOT_FOUND;
  unsigned long messageTime = 0;
  bool messageVisible = false;
  bool initialLayoutFailed = false;
  ButtonNavigator buttonNavigator;
};
