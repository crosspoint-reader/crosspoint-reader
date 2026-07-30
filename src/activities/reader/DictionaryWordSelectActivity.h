#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/Dictionary.h"

class Section;

// Button-driven word selection over the current reader page: the buttons
// that physically lie horizontal step through words in reading order, the
// vertical pair jumps rows (see wasPressedVisual — in landscape the front
// Left/Right pair and the side buttons trade axes), Back returns to the
// reader. What Confirm does depends on the mode:
//  - Dictionary: release looks the word up in DictionaryDefinitionActivity.
//  - Highlight: release anchors a passage selection; the next release saves
//    the anchored range as a markdown highlight (HighlightStore).
//  - DictionaryHighlight: long-press release looks up, short release
//    anchors/saves a highlight. Back cancels an active selection first.
// When a Section is supplied, an anchored selection can keep extending past
// the last word onto the following page(s) of the same chapter.
class DictionaryWordSelectActivity final : public Activity {
 public:
  enum class Mode : uint8_t { Dictionary, Highlight, DictionaryHighlight };

  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int marginLeft, int marginTop,
                                        Mode mode = Mode::Dictionary, std::string bookTitle = {},
                                        std::string chapterTitle = {}, Section* section = nullptr, int pageIndex = 0)
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop),
        mode(mode),
        bookTitle(std::move(bookTitle)),
        chapterTitle(std::move(chapterTitle)),
        section(section),
        originalPageIndex(pageIndex),
        sectionPageIndex(pageIndex) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Screen box of one selectable word. `text` points into the owned Page's
  // TextBlock arena (NUL-terminated), valid for this activity's lifetime.
  struct WordBox {
    int16_t x;
    int16_t y;
    int16_t width;
    uint16_t row;
    const char* text;
    EpdFontFamily::Style style;
    // Hyphenation chain links (word indices, -1 = none), set by
    // detectHyphenJoins(): a word the layout split across lines is selected,
    // highlighted and looked up as one unit. joinedSuffix points to the
    // fragment continuing this word on the next row; joinedPrefix back.
    int16_t joinedSuffix = -1;
    int16_t joinedPrefix = -1;
  };

  enum class Popup : uint8_t { None, Busy, NotFound, Error, Saved };

  // On-screen (visual) directions, resolved to physical buttons per orientation.
  enum class VisualDir : uint8_t { Left, Right, Up, Down };

  void extractWords();
  void buildReadingOrder();
  void detectHyphenJoins();
  int canonicalIndex(int idx) const;
  int selectionEndPos(int hi) const;
  int closestInRow(uint16_t row, int centerX) const;
  void moveVertical(int direction);
  void performLookup();
  void handleConfirmRelease();
  void toggleHighlight();
  bool saveHighlight();
  bool drawHighlightWithSnapshot();
  void drawHints() const;
  bool wasPressedVisual(VisualDir dir) const;
  void paintWordBox(int idx, bool highlighted, int rangeLo, int rangeHi);
  void resetCursorToMiddle();
  bool rowIsRtl(uint16_t row) const;
  bool handleCrossPageNavigation();
  bool advancePage();
  bool retreatPage();
  void resetCarried();
  bool showPage(int pageIndex);

  std::unique_ptr<Page> page;
  const int marginLeft;
  const int marginTop;
  const Mode mode;
  const std::string bookTitle;
  const std::string chapterTitle;
  int fontId = 0;
  int lineHeight = 0;

  std::vector<WordBox> words;
  int selected = 0;
  uint16_t rowCount = 0;

  // TextBlock stores each line's words in visual (left-to-right) order; the
  // logical order is discarded at layout time. These map between the two so
  // passage selection follows the text on RTL (e.g. Arabic) pages:
  // readingOrder[pos] = word index, readingPos[idx] = reading position.
  std::vector<uint16_t> readingOrder;
  std::vector<uint16_t> readingPos;

  // Passage selection (highlight modes): index of the word anchoring the
  // active selection, -1 when none. drawnLo/drawnHi is the reading-position
  // range whose highlight boxes are currently painted in the framebuffer
  // (-1 = unknown, the next render must repaint the full page).
  int anchor = -1;
  int drawnLo = -1;
  int drawnHi = -1;

  // Cross-page selection (only when section != nullptr): the reader's section
  // outlives this activity, so the raw pointer stays valid. carriedText holds
  // the selected words of pages already scrolled past; carriedLens records
  // its length before each page advance so retreating can truncate it.
  // firstPageSelStart is the selection's start reading-position on the page
  // where it was anchored, restored when retreating all the way back.
  Section* section;
  const int originalPageIndex;
  int sectionPageIndex;
  std::string carriedText;
  std::vector<size_t> carriedLens;
  int firstPageSelStart = -1;
  // Bounds the carried text (~2 KB per page) on a 380 KB-RAM device.
  static constexpr size_t MAX_CARRIED_PAGES = 8;

  Dictionary dict;
  bool dictOpenAttempted = false;
  bool dictOpenOk = false;

  Popup popup = Popup::None;
  StrId popupMsg = StrId::STR_DICT_NOT_FOUND;
  unsigned long popupTime = 0;

  // Differential highlight repaint: the pixels under the current highlight
  // box, so a cursor move restores them and repaints only the two affected
  // boxes instead of re-running the full two-pass page render (which also
  // reloads every SD-font glyph on the page). snapshotIdx is the word whose
  // under-pixels are saved; -1 means the framebuffer no longer holds a clean
  // page (popup drawn, sub-activity shown) and the next render must be full.
  // 8 KB covers the union box of a two-row hyphenation chain at the largest
  // built-in line height in landscape (~100 B/row); longer chains fall back
  // to a full repaint via the saved=false path.
  static constexpr size_t SNAPSHOT_CAPACITY = 8192;
  std::unique_ptr<uint8_t[]> snapshot;
  int16_t snapshotX = 0;
  int16_t snapshotY = 0;
  int16_t snapshotW = 0;
  int16_t snapshotH = 0;
  int snapshotIdx = -1;

  // The activity is entered while Confirm is still held (long-press trigger):
  // ignore the stale release until a fresh press is seen.
  bool confirmPressSeen = false;
};
