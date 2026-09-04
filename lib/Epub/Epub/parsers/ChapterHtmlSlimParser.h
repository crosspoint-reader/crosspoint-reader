#pragma once

#include <HalStorage.h>
#include <expat.h>

#include <array>
#include <climits>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Epub/FootnoteEntry.h"
#include "Epub/ParsedText.h"
#include "Epub/blocks/ImageBlock.h"
#include "Epub/blocks/TextBlock.h"
#include "Epub/css/CssParser.h"
#include "Epub/css/CssStyle.h"

class Page;
class GfxRenderer;
class Epub;

#define MAX_WORD_SIZE 200

class ChapterHtmlSlimParser {
  std::shared_ptr<Epub> epub;
  const std::string& filepath;
  GfxRenderer& renderer;
  std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t)> completePageFn;
  std::function<void()> popupFn;  // Popup callback
  bool imagePopupFired = false;   // popupFn fired for the first image probe (single-shot)
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  int boldUntilDepth = INT_MAX;
  int italicUntilDepth = INT_MAX;
  // buffer for building up words from characters, will auto break if longer than this
  // leave one char at end for null pointer
  char partWordBuffer[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex = 0;
  bool nextWordContinues = false;  // true when next flushed word attaches to previous (inline element boundary)
  // A <br> defers its block-break effect instead of applying it immediately, so a
  // doc-pagebreak marker that turns out to hold real content (a paragraph split across
  // a print-page boundary by a bad epub converter) can swallow it instead of it
  // becoming a hard line break. Resolved (materialized or dropped) by
  // materializePendingBr() before anything else can observe currentTextBlock /
  // blockStyleStack, so no snapshot of those is needed alongside the flag; see
  // materializePendingBr() for why that's safe.
  bool pendingBr = false;
  // Set by makePages() when it withholds the "Extra paragraph spacing" half-line gap
  // because the block it just laid out is followed by a <br>-originated block (see
  // makePages()'s nextBlockIsBrLineBreak parameter). Left true means "a line-break
  // <br> is still deciding what it is": if the following block turns out to stay
  // empty (a scene break), startNewTextBlock()'s empty-block reuse path gives the
  // withheld half-line back so the scene break renders exactly as before this
  // suppression existed; if the block instead receives text (a genuine mid-paragraph
  // line break), the flag is simply left to be overwritten by the next <br> and the
  // half-line stays gone, which is the point of the suppression.
  bool brLineBreakSpacingSuppressed = false;
  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  // Ruby text state
  bool inRuby = false;
  int rubyStartWordIndex = -1;
  bool collectingRubyText = false;
  std::string rubyTextBuffer;
  std::unique_ptr<Page> currentPage = nullptr;
  int16_t currentPageNextY = 0;
  int fontId;
  float lineCompression;
  bool extraParagraphSpacing;
  uint8_t paragraphAlignment;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  const CssParser* cssParser;
  bool embeddedStyle;
  uint8_t imageRendering;
  std::string contentBase;
  std::string imageBasePath;
  int imageCounter = 0;

  // Style tracking (replaces depth-based approach)
  struct StyleStackEntry {
    int depth = 0;
    bool hasBold = false, bold = false;
    bool hasItalic = false, italic = false;
    bool hasTextDecoration = false;
    CssTextDecoration textDecoration = CssTextDecoration::None;
    bool hasDirection = false;
    CssTextDirection direction = CssTextDirection::Ltr;
    bool setsParagraphDirection = false;
    bool hasTextAlign = false;
    CssTextAlign textAlign = CssTextAlign::Left;
    bool hasSup = false, sup = false;
    bool hasSub = false, sub = false;
  };
  std::vector<StyleStackEntry> inlineStyleStack;
  std::vector<BlockStyle> blockStyleStack;  // accumulated block styles from open ancestor elements
  CssStyle currentCssStyle;
  bool effectiveBold = false;
  bool effectiveItalic = false;
  CssTextDecoration effectiveTextDecoration = CssTextDecoration::None;
  bool effectiveDirectionDefined = false;
  CssTextDirection effectiveDirection = CssTextDirection::Ltr;
  bool effectiveTextAlignDefined = false;
  CssTextAlign effectiveTextAlign = CssTextAlign::Left;
  bool effectiveSup = false;
  bool effectiveSub = false;
  static constexpr size_t MAX_GRID_TABLE_COLUMNS = 4;
  static constexpr size_t MAX_GRID_TABLE_CELL_WORDS = 32;
  static constexpr size_t MAX_GRID_TABLE_CELL_BYTES = 512;
  int tableDepth = 0;
  bool insideTableCell = false;
  bool tableRowStacked = false;
  bool tableRowRtl = false;
  uint16_t tableRowsSpannedRemaining = 0;
  size_t tableCellTextBytes = 0;
  std::vector<std::unique_ptr<ParsedText>> tableRowCells;
  std::array<std::vector<std::shared_ptr<TextBlock>>, MAX_GRID_TABLE_COLUMNS> tableCellLines;
  std::vector<uint32_t> tableLineVisibleOffsets;
  bool listItemBulletOnly = false;  // true when currentTextBlock has only the <li> bullet

  // Anchor-to-page mapping: tracks which page each HTML id attribute lands on
  int completedPageCount = 0;
  std::vector<std::pair<std::string, uint16_t>> anchorData;
  std::string pendingAnchorId;          // deferred until after previous text block is flushed
  std::vector<std::string> tocAnchors;  // the list of anchors that are TOC chapter boundaries
  uint16_t xpathParagraphIndex = 0;
  uint16_t xpathListItemIndex = 0;
  // Canonical reading-position counter: zero-based Unicode codepoints in visible
  // <body> text. Token offsets flow through line breaking so every completed page
  // records the first source character it renders.
  uint32_t visibleTextOffset = 0;
  uint32_t partWordVisibleOffset = 0;
  uint32_t currentPageVisibleOffset = 0;
  bool currentPageVisibleOffsetSet = false;
  bool insideBody = false;
  bool htmlEnded_ = false;
  bool syntheticCharacterData = false;
  uint16_t nonVisibleTextDepth = 0;

  // Footnote link tracking
  bool insideFootnoteLink = false;
  int footnoteLinkDepth = -1;
  uint8_t currentFootnoteLinkId = 0;
  FootnoteEntry currentFootnote = {};
  int currentFootnoteLinkTextLen = 0;
  std::vector<std::pair<int, FootnoteEntry>> pendingFootnotes;  // <wordIndex, entry>
  int wordsExtractedInBlock = 0;

  // Pagebreak marker capture: badly-converted epubs (Calibre) sometimes wrap real
  // paragraph text inside a role="doc-pagebreak" / epub:type="pagebreak" element
  // instead of leaving it empty. Rather than skip the whole subtree, its text is
  // captured speculatively and replayed as normal content if it turns out to be
  // more than a page-number label. Fixed-size, no heap allocation: this element
  // should almost always be empty or hold a couple of digits.
  static constexpr int PAGEBREAK_LABEL_SIZE = 16;
  static constexpr int PAGEBREAK_CAPTURE_SIZE = 32;
  bool pagebreakCapturing = false;
  int pagebreakCaptureDepth = -1;                  // depth (pre-increment) recorded at the marker's startElement
  char pagebreakLabel[PAGEBREAK_LABEL_SIZE] = {};  // aria-label/title, truncated silently
  char pagebreakCaptureBuffer[PAGEBREAK_CAPTURE_SIZE] = {};
  int pagebreakCaptureLen = 0;
  // visibleTextOffset at the moment capture began. characterData() counts visible
  // codepoints unconditionally, ahead of the keep/drop decision, so by replay time
  // the running counter has already moved past the captured text; replays rewind to
  // this value first so replayed words get the marker's start offset, not its end.
  uint32_t pagebreakCaptureStartOffset = 0;

  // Resumable parse state. The one-shot parseAndBuildPages() drives these
  // internally; the incremental section builder drives them across render ticks
  // so a large single chapter can yield between pages instead of blocking the UI
  // until the whole thing is laid out. parseFile_ and the expat parser stay alive
  // for the lifetime of the parse so it can be paused and resumed at buffer
  // boundaries.
  XML_Parser xmlParser_ = nullptr;
  HalFile parseFile_;
  uint32_t parseStartTime_ = 0;

  void updateEffectiveInlineStyle();
  void startNewTextBlock(const BlockStyle& blockStyle);
  void flushPendingAnchor();
  void flushPartWordBuffer();
  void fallbackTableRowToStacked();
  void closeTableCell();
  void finishTableRow();
  void addTableRowSeparator();
  void materializePendingBr();
  void setCurrentPageVisibleOffset(uint32_t offset);
  // Lays out currentTextBlock and adds it to the current page. nextBlockIsBrLineBreak
  // is true when the caller (startNewTextBlock) is about to start a block created by a
  // <br> that has text before it: the two blocks are consecutive lines of one paragraph,
  // not a paragraph boundary, so the "Extra paragraph spacing" half-line gap normally
  // added after every block is withheld (see brLineBreakSpacingSuppressed). Defaults to
  // false so the finishParse() trailing-page call is unaffected.
  void makePages(bool nextBlockIsBrLineBreak = false);
  static EpdFontFamily::Style fontStyleForTextDecoration(CssTextDecoration decoration);
  static void applyDirectionToEntry(StyleStackEntry& entry, const CssStyle& css);
  static void applyTextDecorationToEntry(StyleStackEntry& entry, const CssStyle& css);
  static void applyVerticalAlignToEntry(StyleStackEntry& entry, const CssStyle& css);
  void pushTableTextStyleEntry(const CssStyle& cssStyle);
  void pushDecorationStyleEntry(CssTextDecoration defaultDecoration, const CssStyle& cssStyle);
  void emitHorizontalRule(const BlockStyle& blockStyle);
  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL defaultHandlerExpand(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

 public:
  explicit ChapterHtmlSlimParser(
      std::shared_ptr<Epub> epub, const std::string& filepath, GfxRenderer& renderer, const int fontId,
      const float lineCompression, const bool extraParagraphSpacing, const uint8_t paragraphAlignment,
      const uint16_t viewportWidth, const uint16_t viewportHeight, const bool hyphenationEnabled,
      const bool focusReadingEnabled,
      const std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t)>& completePageFn,
      const bool embeddedStyle, const std::string& contentBase, const std::string& imageBasePath,
      const uint8_t imageRendering = 0, std::vector<std::string> tocAnchors = {},
      const std::function<void()>& popupFn = nullptr, const CssParser* cssParser = nullptr)

      : epub(epub),
        filepath(filepath),
        renderer(renderer),
        fontId(fontId),
        lineCompression(lineCompression),
        extraParagraphSpacing(extraParagraphSpacing),
        paragraphAlignment(paragraphAlignment),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled),
        completePageFn(completePageFn),
        popupFn(popupFn),
        cssParser(cssParser),
        embeddedStyle(embeddedStyle),
        imageRendering(imageRendering),
        contentBase(contentBase),
        imageBasePath(imageBasePath),
        tocAnchors(std::move(tocAnchors)) {}

  ~ChapterHtmlSlimParser();

  // One-shot parse: builds every page before returning (begin + step* + finish).
  bool parseAndBuildPages();

  // Resumable parse, for the incremental section builder. Drive as:
  //   if (!beginParse()) fail;
  //   loop: switch (parseStep()) { More: keep going / yield; Done: finishParse(); Error: abortParse(); }
  // Pages are emitted via completePageFn as they complete during parseStep(), so
  // the caller can stop once enough pages are built and resume on a later tick.
  enum class ParseStatus { More, Done, Error };
  bool beginParse();
  ParseStatus parseStep();
  bool finishParse();  // flush the trailing page and tear down; returns true
  void abortParse();   // tear down without flushing (error / abandon)

  void addLineToPage(std::shared_ptr<TextBlock> line, uint32_t visibleOffset);
  const std::vector<std::pair<std::string, uint16_t>>& getAnchors() const { return anchorData; }

  // Byte progress of the in-flight parse, used to estimate a still-building section's total page
  // count (a giant single-spine book never fully lays out, so its real count is unknown). Valid
  // between beginParse() and finishParse()/abortParse().
  size_t parseBytesConsumed() { return parseFile_ ? parseFile_.position() : 0; }
  size_t parseTotalBytes() { return parseFile_ ? parseFile_.size() : 0; }
};
