#pragma once

#include <BufferedFileIO.h>
#include <BuildArena.h>
#include <ChunkedVector.h>
#include <HalStorage.h>
#include <Print.h>
#include <SaxParser/SaxParser.h>

#include <array>
#include <climits>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../BlockFontFamilies.h"
#include "../EpubImageManifest.h"
#include "../FontSizeLadder.h"
#include "../FootnoteEntry.h"
#include "../FootnotePreviews.h"
// Page.h, not just a forward declaration: the table layout members below hold TableRow/TableCell
// by value. Only ChapterHtmlSlimParser.cpp and Section.cpp include this header, and both already
// pull Page.h in anyway.
#include "../Page.h"
#include "../ParsedText.h"
#include "../blocks/ImageBlock.h"
#include "../blocks/TextBlock.h"
#include "../css/CssParser.h"
#include "../css/CssStyle.h"
#include "Epub/VisibleText.h"

// Page, PageImage and PageLine all come from ../Page.h above.
class GfxRenderer;
class Epub;

#define MAX_WORD_SIZE 200

// Rough page count for a spine of `inflatedSize` XHTML bytes, used only to pre-size the
// per-page vectors that the build grows one entry at a time (Section's page-offset LUT and
// the parser's paragraph LUT). Deliberately an UNDER-estimate: the point is to remove the
// long doubling ladder from the middle of a parse, not to get the count right. Reserving
// too much would take heap the build needs; reserving a bit too little just leaves one or
// two doublings at the end, which is already the cheap case.
//
// 1 KB of XHTML per page, from device measurement (X4, 2026-08-11): a 16340-byte spine laid
// out to 19 pages (860 B/page), and a 551-byte spine to 1. An earlier 4096 guess reserved 3
// entries for that 19-page spine and left most of the ladder in place.
//
// Erring low is cheap here because MAX_RESERVED_PAGES bounds the whole downside: even a spine
// that saturates it reserves only 2 KB (Section's u32 LUT) and 4 KB (the 8-byte paragraph LUT).
// Past the reserve both tables grow in chunks of at most 1 KB (ChunkedVector), never by a
// doubling of the whole table.
//
// 512 bytes of XHTML per rendered page, not 1024: Strange Pictures' Chapter 3 is 114,201 bytes
// and lays out to 180 pages (634 B/page), so the old estimate (111 pages) had both LUTs doubling
// at page 112 -- a 1.8 KB and a 0.9 KB allocation mid-parse, on a heap that a borrowed build
// runs down to ~4 KB contiguous by then (device run 10). Overestimating costs 4 B + 8 B per
// unused entry; underestimating costs a doubling while the heap is at its lowest.
inline constexpr size_t estimatePagesForSpine(const size_t inflatedSize) {
  constexpr size_t XHTML_BYTES_PER_PAGE = 512;
  constexpr size_t MAX_RESERVED_PAGES = 512;
  const size_t pages = inflatedSize / XHTML_BYTES_PER_PAGE;
  if (pages > MAX_RESERVED_PAGES) return MAX_RESERVED_PAGES;
  return pages;
}

class BuildArena;  // lib/Memory -- see setBuildArena

class ChapterHtmlSlimParser final : public Print {
  std::shared_ptr<Epub> epub;
  GfxRenderer& renderer;
  // Takes each finished page. False: the consumer could not take it (no room to index it), and
  // the parse stops there -- see emitPage.
  std::function<bool(std::unique_ptr<Page>)> completePageFn;
  std::function<void(int)> progressFn;  // Progress callback (0-100)
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  // Skip character data (words only; elements, images and spacing proceed) below this depth:
  // synthetic zero-height spacer <p>, and elements whose text is transparent (color /
  // -webkit-text-fill-color: transparent, alpha-zero colours). Single slot, shallowest wins.
  int skipTextUntilDepth = INT_MAX;
  // Open elements with a background picture, innermost last (see noteBackgroundImage). A block
  // that carries one often ends in a run of <br/> that only made room for the picture behind
  // the text -- Alice's rabbit-hole table ends its cell with 22 -- so inside such a scope a run
  // of line breaks is cut to two; brRunInBackground_ counts the current run.
  struct BackgroundImageScope {
    int depth;
    std::string path;
  };
  std::vector<BackgroundImageScope> backgroundImageScopes_;
  uint8_t brRunInBackground_ = 0;
  int boldUntilDepth = INT_MAX;
  int italicUntilDepth = INT_MAX;
  int underlineUntilDepth = INT_MAX;
  int strikethroughUntilDepth = INT_MAX;
  int preUntilDepth = INT_MAX;  // set when inside a <pre> element; enables \n → line-break handling
  int svgDepth = 0;             // nesting counter for <svg> elements; text inside SVG is skipped (path data etc.)
  // buffer for building up words from characters, will auto break if longer than this
  // leave one char at end for null pointer
  char partWordBuffer[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex = 0;
  // The tokens partWordBuffer's CJK run will become: its CJK codepoints, plus one for each run of
  // non-CJK characters after the first CJK codepoint. consumeText flushes the run at
  // kCjkRunCodepoints of them.
  int partWordCjkCodepoints_ = 0;
  // The last codepoint added to partWordBuffer was CJK, so a non-CJK character after it starts a
  // run that counts toward partWordCjkCodepoints_.
  bool partWordLastWasCjk_ = false;
  // The last codepoint of the last word flushed, kept across whitespace (set by flushPartWordBuffer).
  uint32_t lastTextCodepoint_ = 0;
  // A whitespace run with a line break in it followed unspaced CJK text or a quotation mark
  // (lastTextCodepoint_). If the next word starts with unspaced CJK, or with a quote after unspaced
  // CJK (flushPartWordBuffer decides), the run vanishes instead of becoming a space (CSS Text 3
  // segment-break rule; in CJK text the quotes are CJK punctuation): hard-wrapped Chinese and
  // Japanese sources break lines mid-sentence.
  bool cjkSegmentBreakPending_ = false;
  // KOReader sync content offsets (docs/contributing/koreader-synchronization.md).
  // visibleTextOffset_ counts VisibleText.h's visible bytes of the source text the SAX parser has
  // delivered: inside <body>, outside head/script/style, before any layout decision. The mappers
  // count the same stream with the same rule, so a page's recorded start means the same to both.
  uint32_t visibleTextOffset_ = 0;
  int nonVisibleTextDepth_ = 0;         // > 0 inside head, script or style
  uint32_t partWordVisibleOffset_ = 0;  // offset of the byte that opened partWordBuffer
  uint32_t currentPageVisibleOffset_ = 0;
  bool currentPageVisibleOffsetSet_ = false;
  // Pages emitted with no element (nothing but spacing), last in paragraphLutPerPage: they start
  // where the next element does, which noteElementOnPage fills in.
  uint16_t pageStartsAwaitingElement_ = 0;
  // Records the page's start from its first element; later calls on the same page are ignored.
  void noteElementOnPage(uint32_t offset);
  // The offset an element placed now stands before. Between SAX chunks that is
  // visibleTextOffset_; inside one the counter already holds the chunk's end, so the text path
  // pins the offset of the word in hand (PinnedPlacement) while it runs code that places
  // elements. UINT32_MAX: nothing pinned.
  uint32_t pinnedPlacementOffset_ = UINT32_MAX;
  uint32_t placementOffset() const {
    return pinnedPlacementOffset_ != UINT32_MAX ? pinnedPlacementOffset_ : visibleTextOffset_;
  }
  struct PinnedPlacement {
    PinnedPlacement(ChapterHtmlSlimParser& p, const uint32_t offset) : parser(p), saved(p.pinnedPlacementOffset_) {
      p.pinnedPlacementOffset_ = offset;
    }
    ~PinnedPlacement() { parser.pinnedPlacementOffset_ = saved; }
    PinnedPlacement(const PinnedPlacement&) = delete;
    PinnedPlacement& operator=(const PinnedPlacement&) = delete;
    ChapterHtmlSlimParser& parser;
    uint32_t saved;
  };
  // The body of characterData: lays `s` out. `baseOffset` is the visible-text offset of s[0];
  // `source` says the bytes are chapter text (the running offset advances) rather than text the
  // parser made up (alt text, a footnote preview), whose words take baseOffset unchanged.
  static void consumeText(void* userData, const char* s, int len, uint32_t baseOffset, bool source);
  bool nextWordContinues = false;  // true when next flushed word attaches to previous (inline element boundary)
  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  std::unique_ptr<Page> currentPage = nullptr;
  int16_t currentPageNextY = 0;
  int16_t lastBlockMarginBottom = 0;  // tracks previous block's marginBottom for CSS margin collapsing

  // CSS page-break-before / page-break-after: a break waiting for the next content. It is taken
  // once the text before it is laid out (applyPendingPageBreak), not at the tag: at a start tag
  // the previous paragraph is still in currentTextBlock, and at an end tag so is the element's own.
  bool pageBreakPending_ = false;
  // Depths of the open elements that break after themselves. Fixed size, no heap; nesting deeper
  // than this is pathological, and a deeper one only loses its own break.
  static constexpr uint8_t kMaxBreakAfterDepth = 4;
  uint8_t breakAfterCount_ = 0;
  int breakAfterDepths_[kMaxBreakAfterDepth] = {};

  // Inline image beside paragraph text (CSS float context)
  // Fixed-size arrays — no heap allocation. Float nesting > 4 is pathological in practice.
  static constexpr int kMaxFloatDepth = 4;
  int floatDepth_ = 0;
  int floatOpenDepths_[kMaxFloatDepth] = {};  // parser depth at which each float was opened
  bool floatOpenSides_[kMaxFloatDepth] = {};  // true = right float, false = left float
  struct PendingInlineImage {
    std::string cachedPath;
    std::string epubEntryPath;  // entry path within the EPUB zip
    int16_t width = 0;
    int16_t height = 0;
    std::string alt;
    bool active = false;
    bool isRight = false;  // true when float: right
    // epubFilePath is not stored — epub->getPath() is read at ImageBlock construction time
    // to avoid a redundant heap copy of a constant string.
  };
  PendingInlineImage pendingInlineImage_;   // active=true when a float-context image is deferred
  PageImage* deferredPageImage_ = nullptr;  // borrowed from currentPage; its yPos needs updating

  // Drop cap: a left-floated span with a large font-size at the very start of a
  // paragraph (<p><span class="first-letter">A</span>ll ...). The letter is captured
  // while the span is open, then rendered as its own top-aligned PageLine with a
  // FloatZone so the paragraph's first lines wrap beside it — the same mechanism
  // used for left-floated inline images.
  static constexpr float kDropCapMinMultiplier = 1.95f;  // spans below this render inline
  static constexpr float kDropCapMaxMultiplier = 4.0f;   // ≈3 text lines tall
  static constexpr int16_t kDropCapGapPx = 6;            // horizontal gap between cap and text
  struct PendingDropCap {
    bool active = false;      // true while capturing the span's text
    int depth = 0;            // parser depth of the drop-cap span (pre-increment)
    float multiplier = 1.0f;  // composed font-size multiplier relative to the body font
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    char text[16] = {};  // drop caps are 1 glyph, occasionally with a leading quote
    int textLen = 0;
    uint32_t visibleOffset = 0;  // offset of the first captured byte
  };
  PendingDropCap pendingDropCap_;
  PageLine* deferredDropCapLine_ = nullptr;  // borrowed from currentPage; its yPos needs updating
  // Offset from the paragraph's first-line top to the cap PageLine's yPos, so the cap's
  // INK top (not its leading-padded ascender top) aligns with the first line's ink top.
  int16_t dropCapYAdjust_ = 0;

  // Active float occupying the current page. A floated image never crosses a page
  // boundary (attachPendingFloatImage page-breaks first if it would not fit), so the
  // float lives entirely within one page. While currentPageNextY is above
  // activeFloatBottom_, makePages() injects this zone into every text block so the
  // caption AND the following paragraphs wrap beside the image — not just the one
  // block the image was attached to. Cleared once layout passes activeFloatBottom_
  // or on a page break. activeFloatBottom_ == 0 means no active float.
  int16_t activeFloatTop_ = 0;
  int16_t activeFloatBottom_ = 0;
  int16_t activeFloatWidth_ = 0;  // image width + gap
  bool activeFloatIsRight_ = false;
  int fontId;
  // Default heading multipliers (index 0=h1, 1=h2, 2=h3) applied when a heading has no
  // explicit CSS font-size; resolveBlockFont() then snaps them to the size ladder.
  static constexpr float kHeadingMultiplier[3] = {1.6f, 1.4f, 1.2f};
  // Sibling-size ladder of the body font (see FontSizeLadder). resolveBlockFont() snaps a
  // block's effective font size to the nearest real font on it; empty = scale-only fallback.
  FontSizeLadder fontSizeLadder_;
  // One non-body font per section: body regular/bold/italic plus one auxiliary regular is
  // exactly the FontDecompressor's four page slots. The first block to resolve off-body
  // claims the slot; blocks that would need a different font keep the scale fallback.
  // A native ladder (a TTF family) has no such budget: its faces are read from the glyph store.
  int32_t auxFontId_ = 0;
  // The book's other font families (see BlockFontFamilies); empty, font-family is not looked at. A block takes one
  // family: its words' when they are all in one, else the family of the element that opened it. Words are tracked
  // from the block's first (blockFamilyState_ 1: all in blockFirstFamily_, 2: in several); a block ends in
  // startNewTextBlock, which starts the next one over.
  BlockFontFamilies fontFamilies_;
  uint32_t effectiveFontFamily_ = 0;  // CssStyle::fontFamily of the words being flushed
  uint32_t blockFirstFamily_ = 0;
  uint32_t blockOwnFamily_ = 0;
  uint8_t blockFamilyState_ = 0;
  // A word in another family than its block's has the font slot of its family (BlockFontFamilies::slotOf), whose
  // face resolveBlockFont puts at the block's size: the block's size before it was resolved, and its own slot, kept
  // for the words a block laid out in parts takes after its first part. 0: currentTextBlock is not resolved yet.
  float wordFontsSize_ = 0;
  uint8_t blockFontSlot_ = 0;
  // EPUBs often set their running prose to a nominal CSS size such as 10pt,
  // 87%, or 0.875em. The reader's selected font size is our body size. Root
  // (html/body) sizes are normalized as inherited context; the main-text
  // baseline comes from tag-level paragraph/list rules so prose maps to 1.0
  // and headings/notes remain proportional to that prose size.
  float rootFontSizeBaseline_ = 1.0f;
  bool hasRootFontSizeBaseline_ = false;
  float mainTextFontSizeBaseline_ = 1.0f;
  bool hasMainTextFontSizeBaseline_ = false;
  float lineCompression;
  bool extraParagraphSpacing;
  uint8_t paragraphAlignment;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  bool hyphenationEnabled;
  // When true, widen the per-word font-size dead zone (±10% snaps publisher <span font-size:0.92em>
  // body wrappers back to native size); when false, only a tight ±3% dead zone is applied.
  bool fontSizeNormalization;
  const CssParser* cssParser;
  EpubImageManifest* imageManifest;
  bool embeddedStyle;
  uint8_t imageRendering;
  std::string contentBase;
  std::string imageBasePath;

  // Style tracking (replaces depth-based approach)
  struct StyleStackEntry {
    int depth = 0;
    bool hasBold = false, bold = false;
    bool hasItalic = false, italic = false;
    bool hasUnderline = false, underline = false;
    bool hasStrikethrough = false, strikethrough = false;
    bool hasSup = false, sup = false;
    bool hasSub = false, sub = false;
    bool hasSmallCaps = false, smallCaps = false;
    bool hasMarginLeft = false;
    int16_t marginLeftPx = 0;  // margin-left in pixels, for span-level poem indents
    // Inline font-size as a percent of the PARENT element's size (em semantics).
    // Nested entries compose multiplicatively in updateEffectiveInlineStyle(); one with
    // fontSizeFromRoot (rem, px, pt) is a percent of the BODY size instead and does not.
    bool hasFontSize = false;
    bool fontSizeFromRoot = false;
    uint8_t fontSizePct = 100;
    // font-family (CssStyle::fontFamily), kept only while the book has other families (fontFamilies_).
    bool hasFontFamily = false;
    uint32_t fontFamily = 0;
  };
  // Every element's own emphasis, blocks included, in document order: an inner element's entry
  // comes later, so it overrides the ones around it, and is popped when the element closes.
  std::vector<StyleStackEntry> inlineStyleStack;
  bool effectiveBold = false;
  bool effectiveItalic = false;
  bool effectiveUnderline = false;
  bool effectiveStrikethrough = false;
  bool effectiveSup = false;
  bool effectiveSub = false;
  bool effectiveSmallCaps = false;
  int16_t effectiveInlineMarginLeft = 0;  // accumulated margin-left from inline span stack
  // Composed inline font-size percent (relative to the block font size) for the
  // words currently being flushed. 100 outside sized spans; clamped to the
  // ParsedText per-word range so it always fits the uint8_t word-size channel.
  uint8_t effectiveSizePct = 100;
  // Streaming table model. A row is buffered only until its </tr>, where it is laid out, packed
  // into the pending fragment and freed. The whole table was buffered until </table> once, because
  // grid layout needs the column count before the first row can be placed -- but our columns are
  // viewportWidth/N regardless of content, so the only thing that ever needed every row was the
  // maximum column count. Widening rows flush the fragment and start a new one at the wider count
  // instead; narrower rows still pad, which keeps a ragged row aligned with its neighbours.
  struct BufferedTableCell {
    std::unique_ptr<ParsedText> text;
    std::string imageSrc;  // first image found in this cell (empty if none)
    std::string imageAlt;
    bool isHeader = false;
    uint8_t colSpan = 1;
  };
  struct BufferedTableRow {
    std::vector<BufferedTableCell> cells;
    bool isHeaderRow = false;   // true when all cells in this row are <th>
    uint8_t effectiveCols = 0;  // sum of colSpan values; tracks actual column footprint
  };
  // One buffered row wrapped into grid cells, sized but not yet placed on a page.
  struct LayoutRow {
    std::vector<TableCell> cells;
    uint16_t height = 0;  // content height + 2 x TABLE_CELL_PADDING
    bool isHeaderRow = false;
    uint8_t renderCols = 0;  // grid columns this row was laid out on (the table's column count)
  };
  // Rows accumulated for the PageTableFragment currently being packed. A fragment carries a single
  // column count and must fit the viewport, so a change in either forces a flush.
  struct TableFragmentPacker {
    std::vector<TableRow> rows;
    uint16_t height = 0;
    uint8_t cols = 0;
    uint16_t totalWidth = 0;
    int16_t xInset = 0;  // left edge of the table box; non-zero when the <table> carries a left inset
    bool hasBorder = true;
    uint32_t firstVisibleOffset = UINT32_MAX;  // first word of the fragment's first row with words
  };
  struct BufferedTable {
    // The row currently being filled. Cleared at every <tr> and freed again at </tr> once the row
    // has been laid out, so a table's resident cost is one row, not one table.
    BufferedTableRow pendingRow;
    TableFragmentPacker packer;
    // Column count for the table as a whole, widened by any row that needs more. Distinct from
    // packer.cols, which is per-fragment and resets on every flush: without this a ragged row
    // landing just after a page break would narrow the table's second half.
    uint8_t columnCount = 0;
    // The table's own box, from the <table> element's resolved CSS margins and padding. Columns
    // divide contentWidth rather than the full viewport, so an indented table stays indented
    // instead of being stretched edge to edge.
    uint16_t contentWidth = 0;
    int depth = 0;          // nesting depth; > 1 means we're inside a nested table
    bool hasBorder = true;  // false when border="0" on the <table> element
    // This table can never be a grid (nested table, rowspan, low heap): every cell from here on
    // is emitted as a paragraph at </td>. Latched -- degrading is one-way.
    bool degraded = false;
    // This ROW has already emitted part of itself as paragraphs, so the rest of it must follow
    // in document order rather than going back into the grid. Cleared at the next <tr>, which is
    // what makes a bad row cost one row instead of the whole table.
    bool rowDegraded = false;
    // Set when a cell pushes the row past MAX_TABLE_COLS. The cell is open and about to receive
    // text, so the degrade is deferred to </td> rather than run with a cell mid-fill.
    bool rowOverflowed = false;
    // Attributed resident bytes held by pendingRow's cells (see MAX_TABLE_ROW_BUFFER_BYTES).
    size_t pendingRowBytes = 0;
    // The table's leading header row, re-emitted at the top of every continuation fragment so a
    // table that spans a page break keeps its column labels. Held in its BUFFERED form and laid
    // out again per fragment, NOT as a laid-out LayoutRow whose TextBlocks are shared between
    // fragments. Sharing them was cheaper but it couples the header's lifetime to every page the
    // table touches, which rules out ever scoping line storage to a page (see the table scratch
    // arena). Re-layout costs one row of work per page break and keeps each fragment's lines
    // owned solely by that fragment. Bounded: one row, captured only if it opens the table, and
    // only while it is short enough to be worth the space it costs each page.
    std::unique_ptr<BufferedTableRow> repeatHeader;
    uint16_t repeatHeaderHeight = 0;    // laid-out height, measured once at capture
    bool repeatHeaderResolved = false;  // set after the table's first row; only that row qualifies
  };
  std::unique_ptr<BufferedTable> currentTable;
  BufferedTableCell* currentTableCell = nullptr;  // non-null while inside <td>/<th>

  struct ListEntry {
    int depth;
    bool isOrdered;
    int counter;
    bool suppressMarker = false;  // true when list-style-type: none
  };
  std::vector<ListEntry> listStack;
  // The open <li>'s marker ("•", "3."), held back until the item has content: the item's block
  // stays empty, so a <p> opening the item merges into it and the marker leads that paragraph's
  // first line instead of a line of its own. Empty when no marker is waiting.
  char pendingListMarker_[16] = {};
  void emitPendingListMarker();

  // Ancestor block widths set via an explicit CSS `width` (e.g. a
  // <div style="width:100px"> wrapper). A percentage image width resolves against the
  // innermost such width, so a width:100% image inside a narrow box stays small
  // (matches KOReader) instead of filling the viewport. depth = parser depth at push
  // (pre-increment); popped in endElement when that scope closes.
  // fixedWidth: the innermost absolute (px/em/pt) width at or above this entry, 0 if every
  // wrapper up the chain is a percentage. That is the column a block image falls back to
  // when a percentage wrapper would shrink it below its native size (see the image path).
  struct ContainerWidthEntry {
    int depth;
    int16_t width;
    int16_t fixedWidth;
  };
  std::vector<ContainerWidthEntry> containerWidthStack_;

  // Horizontal insets (margin + padding, per side) of the block-level elements currently open.
  // Every block nested inside them starts at their sum: layout has no box model, so without
  // this a wrapper's inset would reach only its FIRST child -- the empty-block merge in
  // startNewTextBlock() -- and every later sibling would jump back to the viewport edge. With a
  // hanging indent on the children (verse: div{margin-left:2em} > p{text-indent:-1em}) those
  // siblings then start left of the panel and lose their first glyph (issue #198).
  // depth = parser depth at push (pre-increment); popped in endElement when that scope closes.
  struct BlockInsetEntry {
    int depth;
    int16_t left;
    int16_t right;
  };
  // Insets nested deeper than this are dropped rather than tracked: the total is capped at 4em
  // either way, and a book that nests inset wrappers this deep is pathological, not typographic.
  static constexpr size_t kMaxBlockInsetDepth = 8;
  std::vector<BlockInsetEntry> blockInsetStack_;

  // Fold the insets of every enclosing block-level element into `style`, capping each side at
  // MAX_HORIZONTAL_INSET_EM so deep nesting cannot squeeze the text column away.
  void addAncestorInsets(BlockStyle& style, float emSize) const;

  // Bottom spacing (margin-bottom, padding-bottom) of the block-level elements currently open. A
  // block element's style starts only its FIRST text block -- child blocks and <br> lines follow
  // it, and through startNewTextBlock's empty-block merge a wrapper's style becomes its first
  // child's -- so spacing left on that style landed under the first child instead of after the
  // element. It is held here instead and applied at the end tag to whichever block is current.
  // depth = parser depth at push (pre-increment); popped in endElement when that scope closes.
  struct HeldBottomSpacing {
    int depth;
    int16_t marginBottom;
    int16_t paddingBottom;
  };
  // Deeper nesting keeps its spacing on the style, as before: only elements with spacing are
  // pushed, so eight levels of them open at once is already pathological.
  static constexpr size_t kMaxHeldBottomSpacingDepth = 8;
  std::vector<HeldBottomSpacing> heldBottomSpacing_;
  void holdBottomSpacing(BlockStyle& style, bool floated);
  void applyHeldBottomSpacing(const HeldBottomSpacing& held);

  // Anchor-to-page mapping: tracks which page each HTML id attribute lands on
  int completedPageCount = 0;
  // Resident anchors. Empty on the normal path -- see setAnchorSpillPath -- and used only as
  // the fallback when the spill file could not be opened.
  std::vector<std::pair<std::string, uint16_t>> anchorData;
  // Anchors whose element has been opened but whose first line has not been placed yet. The page
  // an anchor names has to be the page its content lands on, and that is not known when the block
  // STARTS: if the block's first line does not fit on the page in progress, addLineToPage emits
  // that page first and the content goes to the next one. Recording at block start therefore put
  // anything beginning exactly at a page boundary one page early -- the reader arrived at the page
  // BEFORE the note. Held until addLineToPage has placed a line, then recorded against the page
  // that line actually went on.
  //
  // Small by construction: an entry lives only from an element's start tag to its first line, so
  // it holds more than one only for id'd elements that produce no text at all. Past
  // MAX_ANCHORS_AWAITING_LINE the queue is flushed at the current page rather than grown, which
  // is exactly the old behaviour and no worse.
  std::vector<std::string> anchorsAwaitingLine_;
  // Records every queued anchor against `completedPageCount`. Called once a line has been placed
  // (the page is then known) and again at end of parse for anchors that never got one.
  void flushAnchorsAwaitingLine();
  // Holds an anchor until a line settles its page; see anchorsAwaitingLine_.
  void queueAnchorForNextLine(std::string id);
  // Records the id of a <span>, within MAX_SPAN_ANCHORS_PER_CHAPTER and never counted against the
  // general cap: see the definition.
  void recordSpanAnchor(const std::string& id);
  // Span ids taken so far (recordSpanAnchor). The general cap is checked against anchorCount less
  // these, so a chapter whose spans fill their budget keeps every slot it had for the rest.
  uint16_t spanAnchorCount_ = 0;

  std::string anchorSpillPath;
  FsFile anchorSpillFile;
  std::optional<serialization::BufferedFileWriter> anchorSpillWriter;
  uint16_t anchorCount = 0;
  // Latched when a spill write or flush failed. The spill is then a truncated record stream that
  // cannot be trusted, so the map is abandoned rather than half-written -- and this stops the
  // resident fallback from picking up the remaining anchors and producing a map that silently
  // begins in the middle of the chapter.
  bool anchorSpillFailed = false;
  // Appends one anchor, to the spill file when it is open and to anchorData otherwise.
  void recordAnchor(std::string id, uint16_t page);
  // How many anchors this chapter may still record: the spilled cap when the spill is open, the
  // much smaller resident one when it is not. Defined in the .cpp beside the two constants.
  size_t anchorLimit() const;
  std::string pendingAnchorId;  // deferred until after previous text block is flushed
  std::vector<std::string> tocAnchors;

  // External printed-page labels sourced from NCX <pageList> or EPUB 3 nav page-list.
  // Keyed by HTML id (anchor fragment). When the parser encounters an element whose id
  // matches one of these, it records the label as if the element were an inline
  // doc-pagebreak marker. Anchors already labeled this way are not re-recorded if the
  // same element also carries an inline pagebreak attribute.
  //
  // Packed: each entry is "id\0label\0", back to back (memory audit 2026-09, build inventory: the
  // vector of std::string pairs this replaces held ~3.5 KB of heap for the whole build on a
  // 72-anchor chapter; the same entries are ~0.9 KB here). Scanned linearly, as the vector was.
  std::string externalPageBreakAnchorPool;
  uint16_t externalPageBreakAnchorCount = 0;
  size_t externalPageBreakLabelBytes = 0;  // sum of label lengths + NULs, to size pageBreakLabelPool
  // Optional label for the start of this XHTML file (NCX entries with no fragment).
  std::string topOfFilePageLabel;
  bool topOfFilePageLabelEmitted = false;

  // Page break label mapping: stores the printed page label from EPUB pagebreak markers
  // and the section page index where that printed page begins. Packed like the anchors above:
  // the page indices in one vector, the labels NUL-separated in one string, in the same order
  // (was a vector of {uint16_t, std::string}, 28 B per entry on the device before any text).
  std::vector<uint16_t> pageBreakLabelPages;
  std::string pageBreakLabelPool;

  // Paragraph index tracking for XPath-to-page lookup table.
  // Counts <p> sibling indices (1-based, matching XPath convention) during page building.
  // Stored per page in the section cache so that XPath p[N] can be resolved to a page
  // without reparsing, and current page can generate an XPath without reparsing.
  uint16_t xpathParagraphIndex = 0;  // current <p> sibling index (1-based)
  // Running count of <li> elements opened anywhere in the chapter (1-based, any depth).
  // Used by the section LUT so KOReader-supplied list-item XPaths can snap to the exact
  // page on download, the same way <p>-anchored XPaths use xpathParagraphIndex.
  uint16_t xpathListItemIndex = 0;
  int xpathBodyDepth = -1;  // depth of the <body> element (-1 = not yet seen)

  struct ParagraphLutEntry {
    uint32_t visibleTextOffset;  // visible bytes of source text before the page's first element
    uint16_t paragraphIndex;     // 1-based <p> index at page completion
    uint16_t listItemIndex;      // running <li> count at page completion (any depth)
  };
  // One entry per page, held for the whole parse: chunked (at most 128 entries, 1 KB, per
  // block, allocated nothrow) for the same reason as Section's page LUT -- see ChunkedVector.
  using ParagraphLut = ChunkedVector<ParagraphLutEntry, 16, 128, Page::MAX_PAGES_PER_SECTION>;
  ParagraphLut paragraphLutPerPage;  // deep LUT: one entry per page
  // Latched when a page could not be indexed (this table or the consumer's could not grow): the
  // parse stops there, and every page after it is dropped too so the tables stay in step.
  bool pageTableFull_ = false;
  // Drops the page in hand and stops the parse the way a low-heap abort does: the pages already
  // emitted are kept as a truncated section.
  void stopOnFullPageTable();

  // Active parser for streaming. A member so write() can feed it and the abort paths can stop
  // it from inside a callback.
  SaxParser saxParser_;
  BuildArena* buildArena_ = nullptr;  // see setBuildArena
  // The page under construction's block in buildArena_: every line's TextBlock bytes are
  // bump-allocated in it (ParsedText::lineArena_) and it is rewound once the page has been
  // serialised, so a page's lines cost the heap nothing. Opened by the before-line hook, which
  // is also where the page-fit test runs so the block is always the landing page's. Nested
  // above the section's feed-chunk block and below nothing that outlives the page (a mid-build
  // draw's font-slot scope opens and closes between two slices): strictly LIFO.
  BuildArena::Block pageBlock_;

  // Streaming state for the Print-derived parsing API.
  size_t totalStreamSize = 0;
  size_t bytesStreamed = 0;
  int lastReportedProgress = -1;
  int progressStepPercent = 0;
  bool progressUiEnabled = true;
  bool streamFailed = false;
  // Set when the heap gate refused an image-header read (see imageHeaderDegraded()).
  bool imageHeaderSkippedForHeap = false;
  // Set when any table row was demoted to paragraphs (degradeRow), for any reason: the pages
  // are usable but the cache holds a layout the heap chose, not the book. See tableRowDegraded().
  bool tableRowDegradedAny_ = false;
  // Fixed-capacity limits this parse ran into that changed its output (memory audit 2026-09,
  // R4). Each is logged at ERR once per parse and the union is latched into the section status
  // byte (Section::isSimplified) -- a deterministic condition, so nothing rebuilds on it; the
  // reader can say "chapter simplified" instead of showing less than the book without a word.
  uint8_t capOverflowFlags_ = 0;
  void noteCapOverflow(uint8_t flag, const char* what);
  // Latches the one-shot font-cache release that recoverHeapForImageHeader() spends.
  bool fontCachesReleasedForImageHeader = false;
  uint32_t streamStartTimeMs = 0;

  // Footnote link tracking
  bool insideFootnoteLink = false;
  int footnoteLinkDepth = -1;
  FootnoteEntry currentFootnote = {};
  size_t currentFootnoteLinkTextLen = 0;
  // Non-owning; the Section's BuildState keeps the lookup alive across build slices.
  // Membership in the book's preview store is the sole expansion gate: it already encodes
  // "this link points at a real note", so no epub:type/same-file checks here. The store is
  // filled for this spine before the parse starts (Section::resolveInlineFootnotePreviews).
  FootnotePreviews::Lookup* inlineFootnotePreviews = nullptr;
  std::string pendingInlineFootnotePreview;
  std::vector<std::pair<int, FootnoteEntry>> pendingFootnotes;  // <wordIndex, entry>
  int wordsExtractedInBlock = 0;
  bool bionicReadingEnabled = false;
  bool layoutFailed = false;

  // Per-chapter caches: resolveStyle and parseInlineStyle are called for every HTML element;
  // caching by (tag|classAttr) and styleAttr avoids repeated string operations and hash lookups.
  std::unordered_map<std::string, CssStyle> cssStyleCache_;
  std::unordered_map<std::string, CssStyle> inlineStyleCache_;
  // Both are pure memos of deterministic resolves, so they are BOUNDED: once a memo holds
  // kStyleMemoMaxEntries, later misses are resolved and used but not inserted, which changes
  // nothing but time. A reflowable chapter has tens of unique keys and never reaches the cap.
  // A PDF-derived chapter does not: every text run carries its own left/top in style="...", so
  // every key is unique, the hit rate is 0%, and an unbounded memo grew one ~250 B node per
  // element -- ~120 KB on a 66 KB chapter of Deckhand, 70% of the build's peak, against ~29 KB
  // of contiguous heap while reading on the C3. The node cost is the key string plus the
  // CssStyle payload, so it does not shrink on a 32-bit target.
  //
  // On an arena build the cap is much lower. Two full memos are up to 32 KB of heap held for the
  // whole chapter, and a borrowed build has ~40 KB to begin with: the Roosevelt appendix (many
  // class combinations and per-cell inline styles) went from 38 KB free after extraction to
  // 12 KB at its first table row with nothing else growing (device run 13). The memo buys little
  // there anyway -- the ruleset is arena-resident and a resolve is an in-RAM hash lookup -- so
  // eight entries keep the common keys hot and bound the heap at a few KB.
  static constexpr size_t kStyleMemoMaxEntries = 64;
  static constexpr size_t kStyleMemoMaxEntriesArena = 8;
  bool styleMemoHasRoom(const std::unordered_map<std::string, CssStyle>& memo) const {
    return memo.size() < (buildArena_ != nullptr ? kStyleMemoMaxEntriesArena : kStyleMemoMaxEntries);
  }
  // parseInlineStyle(styleAttr), memoised while there is room. Returned by value: CssStyle is a
  // flat struct with no heap members, and a reference into the memo would dangle on the
  // no-insert path.
  CssStyle inlineStyleFor(const std::string& styleAttr);
  // cssParser->resolveStyle("img", classAttr) under the key "img|classAttr", memoised while
  // there is room. Requires cssParser != nullptr.
  CssStyle resolvedImgStyle(const std::string& classAttr);

  // Descendant and child selectors: which of the sheet's compounds each open element matches
  // (CssParser::compoundMask), indexed by depth, so an element is matched against its ancestors
  // without keeping their names. Fixed size, written in place: no heap per element. An element
  // deeper than the stack resolves with no ancestry, so descendant and child rules skip it. Every
  // element that opens below the CSS resolve records its slot; the SVG paths, which return
  // earlier, clear theirs so a descendant never sees a stale sibling's mask.
  static constexpr size_t kMaxCssAncestors = 32;
  uint64_t cssAncestorMasks_[kMaxCssAncestors] = {};
  // The ancestry of the element opening at `depth` (whose own mask it computes).
  CssParser::Ancestry cssAncestry(const std::string& tagName, const std::string& classAttr,
                                  const std::string& idAttr) const;

  // Default size for superscript/subscript text, percent of the surrounding size.
  // Sup/sub scaling flows through the ordinary per-word size channel (the SUP/SUB
  // style bits only shift the baseline at render time); an explicit CSS font-size
  // on the element overrides this default.
  static constexpr uint8_t kSupSubDefaultSizePct = 50;

  void updateEffectiveInlineStyle();
  // Fold an element's CSS font-size (multiplier relative to its parent) into an inline
  // style-stack entry.
  static void applyCssFontSizeToEntry(StyleStackEntry& entry, const CssStyle& cssStyle);
  // Apply kSupSubDefaultSizePct when the entry resolves to sup/sub. Call BEFORE
  // applyCssFontSizeToEntry so publisher CSS (e.g. `.sup { font-size: 0.7em }`) wins.
  static void applySupSubDefaultSize(StyleStackEntry& entry);
  // Fold an element's CSS vertical-align into an inline style-stack entry. `baseline`
  // explicitly cancels an inherited sup/sub rather than being ignored.
  static void applyVerticalAlignToEntry(StyleStackEntry& entry, const CssStyle& cssStyle);
  // Fold an element's CSS font-weight, font-style, text-decoration and font-variant into an
  // inline style-stack entry. `text-decoration: none` cancels an inherited underline/strike.
  static void applyCssEmphasisToEntry(StyleStackEntry& entry, const CssStyle& cssStyle);
  // Fold an element's font-family into an entry, while the book has other families; true when it did.
  bool applyCssFontFamilyToEntry(StyleStackEntry& entry, const CssStyle& cssStyle) const;
  // A block's (or heading's) own emphasis, pushed at its depth so the blocks nested inside it
  // inherit it and its end tag pops it. Nothing is pushed for a block that sets none.
  void pushBlockStyleEntry(const CssStyle& cssStyle);
  // Notes the family of a word going into currentTextBlock (see fontFamilies_).
  void noteWordFamily();
  void initializeFontSizeBaseline();
  void observeFontSizeBaseline(const char* tagName, const CssStyle& cssStyle);
  CssStyle normalizeFontSizeForElement(const char* tagName, const CssStyle& cssStyle) const;
  // `block` is the ParsedText about to be laid out (null when there is none): a block whose word
  // vectors could not grow (ParsedText::wordGrowthRefused) aborts the parse here, on the same
  // partial-cache path as the heap floors, instead of laying out a truncated paragraph.
  bool ensureHeapForTextLayout(const char* phase, const ParsedText* block);
  // Whether the heap can afford the ~32 KB inflate ring a ZIP image-header read needs. Checked at
  // the call site, never latched: the heap recovers between pages, and a single dip must not
  // disable images for the rest of the chapter (that result gets baked into the section cache).
  bool heapAllowsImageHeaderRead() const;
  // Gate for the streaming header walk of a deferred image: walkBytes is what the walk will
  // allocate (EpubImageManifest::deferredWalkBytes — one contiguous inflate ring of up to 32 KB
  // plus its read chunk), so contiguous heap is the hard bar.
  // Contiguous heap a deferred image's walk may take right now (never 0: 0 would mean "no
  // limit" to the manifest). See EpubImageManifest::resolveDeferredNow.
  size_t imageWalkBudget() const;
  // Last resort before an image degrades to alt text: drop the rebuildable SD-font
  // glyph caches, which are usually what is holding the contiguous space the header
  // read needs. One shot per parse — once they are gone there is nothing left to
  // reclaim, and re-releasing per image would only thrash the re-read. Returns true
  // if caches were actually released, i.e. it is worth re-testing the gate.
  bool recoverHeapForImageHeader();
  void startNewTextBlock(const BlockStyle& blockStyle);
  // The text before this point and the text after it are not in one inline formatting context, so
  // a line break between them stays a space (cjkSegmentBreakPending_).
  void endCjkSegment() {
    cjkSegmentBreakPending_ = false;
    lastTextCodepoint_ = 0;
  }
  void clearSpentBlockHeadingStyle();
  // Depth of the header/block element whose start tag set up the current text block, or -1 once
  // that element has closed. A <br> inside it splits the element into more blocks, which keep its
  // font size; after it has closed, what it leaves behind must not lend that size to a sibling.
  int currentBlockOwnerDepth_ = -1;
  // The current block's own font-size is body-relative (rem/px/pt): its size then ignores the
  // sizes of the inline containers it sits in. Set where a block or heading starts, cleared when
  // it ends, as the single block-style slot did before blocks went onto the style stack.
  bool currentBlockSizeFromRoot_ = false;
  // The open heading and the alignment and size its block was given. A block opening inside it is
  // heading text and takes both (applyHeadingScope): otherwise only the first such block got them,
  // by merging into the heading's still-empty block. INT_MAX when no heading is open.
  int headingDepth_ = INT_MAX;
  CssTextAlign headingAlignment_ = CssTextAlign::Center;
  float headingFontSizeMultiplier_ = 1.0f;
  void applyHeadingScope(BlockStyle& blockStyle, const CssStyle& cssStyle) const;
  // The <span> laid out as a block because the book styles it display:block (#388), and the
  // alignment and size of the block it interrupted: text after the span, still inside the same
  // element, resumes on a line of its own in that style. One at a time -- a display:block span
  // inside another stays inline. depth is INT_MAX when none is open.
  struct BlockSpan {
    int depth = INT_MAX;
    int interruptedOwnerDepth = -1;
    CssTextAlign alignment = CssTextAlign::Justify;
    bool textAlignDefined = false;
    bool fontResolved = false;
    float fontSizeMultiplier = 1.0f;
    int32_t headingFontId = 0;
  };
  BlockSpan blockSpan_;
  void startBlockSpan(const CssStyle& cssStyle, BlockStyle blockStyle);
  void endBlockSpan();
  bool heapAllowsTableRowLayout() const;
  bool flushPartWordBuffer();
  void makePages();
  // Puts currentTextBlock's top spacing on the current page, once per logical paragraph. Both
  // places that lay the block out call it first: makePages() and the long-block split in
  // flushPartWordBuffer(). Returns false, with layoutFailed set, when no page could be allocated.
  bool applyBlockTopSpacing();
  // Called at </tr>: lay the pending row out, pack it into the fragment, and free its cells.
  // Degrades the row to paragraphs if it cannot be a grid row. No-op for an empty row.
  void commitPendingRow();
  // Wrap one buffered row into grid cells at `columnCount` columns. Returns false when the row
  // cannot be a grid row at all -- an unsupported colspan, a cell needing more lines than the grid
  // carries, or a cell taller than the viewport -- leaving `out` unusable and the row's cell text
  // untouched, so the caller can still emit it as paragraphs. The caller chooses how far to fall
  // back; nothing is written to a page from here.
  bool layoutTableRow(BufferedTableRow& bufRow, uint8_t columnCount, LayoutRow& out);
  // Emit the packed rows as one PageTableFragment, page-breaking first if it does not fit, and
  // reset the packer. No-op when nothing is packed.
  void flushTableFragment(TableFragmentPacker& packer);
  // Emit every cell of `row` as a sequential paragraph, releasing each cell's ParsedText as it is
  // laid out. Returns false when the heap guard stopped the parse.
  bool emitRowAsParagraphs(BufferedTableRow& row);
  // Paragraph path only: emit each cell's image as a full-width block image, then drop the cells.
  void emitRowImagesAsBlocks(BufferedTableRow& row);
  // Emit one buffered cell as a paragraph (plus its image, when emitImage) and free the cell's
  // ParsedText before returning. Shared by the row fallback and the degraded path so both
  // produce identical output. Returns false when the heap guard stopped the parse.
  bool emitCellAsParagraph(BufferedTableCell& cell, bool emitImage);
  // Give up the grid for the pending ROW: flush whatever is packed so the fragment closes before
  // the paragraphs, then emit the row's buffered cells (text then images) and latch rowDegraded so
  // the rest of the row streams at </td>. The next <tr> clears it and the grid resumes.
  void degradeRow(const char* reason);
  // Give up the grid for the whole TABLE. degradeRow plus a latch, for triggers that cannot
  // recover on the next row: a nested table, a rowspan, or a heap too low to lay rows out.
  void degradeTable(const char* reason);
  // Degrade the row while a <td> is still open: lifts the open cell out, degrades, then emits the
  // words accumulated so far and hands the cell back empty so parsing continues into it. Needed
  // because degradeRow() clears the row and so can only run between cells -- which never happens
  // in a one-cell row. Returns false when the drain stopped the parse (low heap).
  bool degradeRowAtOpenCell(const char* reason);
  // Called on </td> once degraded: emit and release the just-closed cell immediately.
  void streamClosedCell(BufferedTableRow& row);
  // Resolve an image src to a sized ImageBlock (lazy-extracted from the EPUB), scaled to fit
  // maxWidth/maxHeight. Returns nullptr when the image is unsupported or its dimensions
  // cannot be resolved. The cache path is derived from the archive entry, not from parse order.
  std::unique_ptr<ImageBlock> buildCellImage(const std::string& src, const std::string& alt, uint16_t maxWidth,
                                             uint16_t maxHeight);
  // buildCellImage for a path already resolved inside the EPUB. `nativeWidth`/`nativeHeight`, when
  // given, receive the picture's own size.
  std::unique_ptr<ImageBlock> buildImageFromEpubPath(const std::string& resolvedPath, const std::string& alt,
                                                     uint16_t maxWidth, uint16_t maxHeight,
                                                     uint16_t* nativeWidth = nullptr, uint16_t* nativeHeight = nullptr);
  // Background pictures (CssParser::backgroundImageFor). The renderer cannot draw text over a
  // picture, so a block's background picture becomes a picture of its own, placed after the
  // block's content: noteBackgroundImage opens the scope on the element, endElement places the
  // picture when the element closes. See BackgroundImageScope.
  void noteBackgroundImage(const char* name, const std::string& classAttr, const std::string& idAttr,
                           const std::string& styleAttr);
  void placeBackgroundImage(const std::string& resolvedPath);
  // Place an already-built ImageBlock as a centered, full-width block element, page-breaking if needed.
  void placeImageBlockAsBlock(std::unique_ptr<ImageBlock> image);
  // Emit currentPage to the consumer while keeping paragraphLutPerPage and completedPageCount
  // in lockstep. Every page break MUST go through this helper; open-coded completePageFn
  // calls risk desynchronising paragraphLutPerPage and failing the size check in Section.cpp.
  void emitPage();
  // Take a pending page break (pageBreakPending_): emit the page if anything is on it. Called
  // where the content before the break has been laid out and the next is about to be placed.
  void applyPendingPageBreak();
  void recordPageBreakLabel(const std::string& label);
  // Attach the pending inline float image to `bs` and place it on the current page.
  // Clears pendingInlineImage_ on return.  No-op if pendingInlineImage_ is not active.
  void attachPendingFloatImage(BlockStyle& bs);
  // Begin drop-cap capture when a left-floated large-font inline element opens at the
  // start of an empty paragraph. Returns true when capture started (the element must
  // then not push an inline style entry).
  bool tryStartDropCapCapture(const CssStyle& cssStyle);
  // Place the captured drop cap: one-word PageLine on the current page plus a FloatZone
  // on the paragraph's block style. Falls back to an inline word when the cap is unusable.
  void finalizePendingDropCap();
  // XML callbacks
  static void startElement(void* userData, const char* name, const char** atts);
  static void characterData(void* userData, const char* s, int len);
  static void defaultHandlerExpand(void* userData, const char* s, int len);
  static void endElement(void* userData, const char* name);
  static void endElementBody(ChapterHtmlSlimParser* self, const char* name);
  std::string abbreviateInlineFootnote(const char* text) const;

 public:
  explicit ChapterHtmlSlimParser(std::shared_ptr<Epub> epub, GfxRenderer& renderer, const int fontId,
                                 const float lineCompression, const bool extraParagraphSpacing,
                                 const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                 const uint16_t viewportHeight, const bool hyphenationEnabled,
                                 const bool fontSizeNormalization, const bool bionicReadingEnabled,
                                 const std::function<bool(std::unique_ptr<Page>)>& completePageFn,
                                 const bool embeddedStyle, const std::string& contentBase,
                                 const std::string& imageBasePath, const uint8_t imageRendering = 0,
                                 std::vector<std::string> tocAnchors = {},
                                 const std::function<void(int)>& progressFn = nullptr,
                                 const CssParser* cssParser = nullptr, EpubImageManifest* imageManifest = nullptr)

      : epub(epub),
        renderer(renderer),
        completePageFn(completePageFn),
        progressFn(progressFn),
        fontId(fontId),
        lineCompression(lineCompression),
        extraParagraphSpacing(extraParagraphSpacing),
        paragraphAlignment(paragraphAlignment),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight),
        hyphenationEnabled(hyphenationEnabled),
        fontSizeNormalization(fontSizeNormalization),
        cssParser(cssParser),
        imageManifest(imageManifest),
        embeddedStyle(embeddedStyle),
        imageRendering(imageRendering),
        contentBase(contentBase),
        imageBasePath(imageBasePath),
        tocAnchors(std::move(tocAnchors)),
        bionicReadingEnabled(bionicReadingEnabled) {}

  ~ChapterHtmlSlimParser() override;

  // Streaming parse lifecycle. Caller flow:
  //   parser.setup(totalInflatedSize);
  //   epub->readItemContentsToStream(href, parser, ...);
  //   parser.finalize();
  // Returns false from setup() on parser allocation failure; check streamSucceeded()
  // after finalize() to detect a parse error mid-stream.
  bool setup(size_t totalInflatedSize);
  bool finalize();
  [[nodiscard]] bool streamSucceeded() const { return !streamFailed; }
  // True when at least one image was dropped to alt text because the heap gate refused its
  // header read — a transient condition, unlike an unreadable or unsupported image. The pages
  // are usable but incomplete, and the caller must not keep them: see the latch site in
  // startElement's image branch.
  [[nodiscard]] bool imageHeaderDegraded() const { return imageHeaderSkippedForHeap; }
  // True when at least one table row was emitted as paragraphs instead of a grid (heap gate,
  // row byte budget, an open-cell overflow). Latched into the section status byte so the
  // reader can rebuild the chapter once when memory allows, instead of keeping the demoted
  // layout for good (memory audit 2026-09, F3/R3).
  [[nodiscard]] bool tableRowDegraded() const { return tableRowDegradedAny_; }
  enum CapOverflow : uint8_t {
    kCapFootnotesPerPage = 1u << 0,   // Page::MAX_FOOTNOTES_PER_PAGE: later links on the page dropped
    kCapAnchorsPerChapter = 1u << 1,  // MAX_ANCHORS_PER_CHAPTER, or no heap for resident ones: ids dropped
    kCapPageElements = 1u << 2,       // Page::MAX_ELEMENTS: a page's tail elements not written
    kCapPageLabels = 1u << 3,         // 65535 printed-page labels, or no heap to grow them: labels dropped
    kCapFootnoteHref = 1u << 4,       // FOOTNOTE_HREF_LEN: a link too long to navigate, kept as text
    kCapSaxDepth = 1u << 5,           // SaxParser kMaxDepth: nesting flattened past 64
    kCapPagesPerSection = 1u << 6,    // Page::MAX_PAGES_PER_SECTION: parse stopped (truncated cache)
  };
  // Union of the CapOverflow bits this parse hit; 0 when every structure fit.
  [[nodiscard]] uint8_t capOverflowFlags() const { return capOverflowFlags_; }
  void setInlineFootnotePreviews(FootnotePreviews::Lookup* lookup) { inlineFootnotePreviews = lookup; }

  // Print interface — fed by Epub::readItemContentsToStream.
  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;

  ParsedText::LineProcessResult addLineToPage(std::unique_ptr<TextBlock> line, bool lineEndsWithHyphenatedWord,
                                              bool suppressHyphenationRetry, uint32_t lineVisibleOffset);
  // Anchors reach the section cache through the spill file, not through this vector: see
  // setAnchorSpillPath. Non-empty only when the spill could not be opened, in which case these
  // are all the anchors there are and the finalizer writes them itself.
  const std::vector<std::pair<std::string, uint16_t>>& getAnchors() const { return anchorData; }
  // Mid-build lookup of an anchor this parse has already recorded: the spill's buffered tail is
  // flushed and the file scanned through a read handle (or the resident fallback vector when
  // there is no spill). False while the anchor has not been reached -- the caller asks again
  // after the next slice. This is what lets a chapter-list jump (a TOC entry with a fragment) or
  // a link into an unbuilt spine show its page as soon as it exists, instead of waiting for the
  // whole build (device run 14: 7.6 s behind the popup for a heading page that existed after
  // 50 ms).
  bool lookupAnchorInActiveBuild(const std::string& id, uint16_t& page);
  // Total anchors recorded, spilled and resident together. This is the count the section
  // cache's anchor map is written with.
  uint16_t getAnchorCount() const { return anchorCount; }
  // Where to stream anchors as they are found. Must be set before setup(); an empty path (or a
  // file that will not open) falls the parser back to holding them in `anchorData`.
  //
  // Anchors are append-only during the parse and read exactly once at the end, in document
  // order, so nothing needs them resident -- and holding them was the single largest
  // chapter-scaled allocation in the build. A 549-anchor chapter (an ordinary endnotes
  // document) grew this vector to a 1024-entry capacity: 28.7 KB held for the whole parse,
  // reached through a 43 KB contiguous peak while the doubling had both blocks live. That is
  // more than the entire ~20 KB the build's heap gates budget for the parse working set, and a
  // plain push_back has no way to fail softly -- under -fno-exceptions it aborts.
  //
  // Streaming makes the cost O(1) in the number of anchors. The bytes written here are exactly
  // the section cache's anchor-map encoding, so the finalizer copies them in verbatim.
  void setAnchorSpillPath(std::string path) { anchorSpillPath = std::move(path); }
  // The build's arena, when the caller has one worth the space (the borrowed secondary
  // framebuffer): setup() places the SAX parser's ~10 KB state in it instead of the heap. A
  // background build runs with ~46 KB of heap, and this state plus the build's other
  // long-lived buffers left ~16 KB for layout, which fragmented to a low-heap abort mid-chapter
  // (X3 2026-09-25, page 67 of 180).
  //
  // Taken in setup(), i.e. at the start of phase (b), and not here at wiring time: wiring
  // happens before extraction, and 10 KB taken then leaves the region too small for the
  // inflate ring plus its grow buffer on a big chapter, which pushes a 33 KB ring onto the
  // heap (host census: arena high-water 45,872 -> 28,576 with the ring evicted). The state is
  // a plain allocation inside the feed-chunk block's scope, so the section must release that
  // block only after finalize() -- Section::runBuildParse does, and its BuildState tears the
  // parser down before the block (memory audit 2026-09, F2a).
  void setBuildArena(BuildArena* arena) { buildArena_ = arena; }
  // The section is about to yield this slice of an incremental build: a mid-build page draw
  // may run before the next feed. See ParsedText::releaseLayoutScratch.
  void onSliceYield() {
    if (currentTextBlock) currentTextBlock->releaseLayoutScratch();
  }
  const std::string& getAnchorSpillPath() const { return anchorSpillPath; }
  size_t pageBreakLabelCount() const { return pageBreakLabelPages.size(); }
  // Heap held by the label record (for the SCT heap trace).
  size_t pageBreakLabelHeapBytes() const {
    return pageBreakLabelPages.capacity() * sizeof(uint16_t) + pageBreakLabelPool.capacity();
  }
  // Calls f(pageIndex, label, labelLength) for every recorded printed-page label, in document order.
  template <typename F>
  void forEachPageBreakLabel(F&& f) const {
    const char* label = pageBreakLabelPool.c_str();
    for (const uint16_t page : pageBreakLabelPages) {
      const size_t len = std::strlen(label);
      f(page, label, len);
      label += len + 1;
    }
  }
  const ParagraphLut& getParagraphLutPerPage() const { return paragraphLutPerPage; }

  // Supplies printed-page labels from NCX <pageList> for this chapter. `anchors` maps
  // HTML id -> label; an entry with an empty id applies to the first page of this file.
  // Filled entry by entry (no intermediate container): begin, one add per anchor, end.
  void beginExternalPageBreakAnchors();
  void addExternalPageBreakAnchor(const std::string& id, const std::string& label);
  void endExternalPageBreakAnchors();

  // Supplies the body font's sibling-size ladder (see FontSizeLadder). Blocks whose
  // effective font size differs from the body resolve to the nearest real font on it.
  void setFontSizeLadder(const FontSizeLadder& ladder) { fontSizeLadder_ = ladder; }
  // Supplies the book's other font families (see BlockFontFamilies); none by default.
  void setFontFamilies(const BlockFontFamilies& families) { fontFamilies_ = families; }

 private:
  // Snap a completed block's effective font size (block multiplier, after uniform per-word
  // folding) to the size ladder: sets headingFontId to the chosen real font and reduces
  // fontSizeMultiplier to the residual. Applies the one-aux-font-per-section budget and is
  // idempotent via bs.fontResolved. Must run before the block's first layout so measured
  // widths, line heights and rendering all use the same (fontId, scale) pair. A block in one
  // of the book's other families (fontFamilies_) takes that family's face at its size instead,
  // outside the budget: the body font scaled would not be its family. Its words in other families get their faces
  // at the block's size in their font slots (resolveWordFont).
  void resolveBlockFont(BlockStyle& bs);
  // The face text in `family` (nullptr: the body's) is set in at `size`, a multiple of the body size: fontId 0 is the
  // body font, with the size left over as the residual. What resolveBlockFont does for a block, and for each family of
  // its words.
  FontSizeLadder::Resolved resolveFamilyFont(const BlockFontFamilies::Family* family, float size);
  // Puts the face of font slot `slot` at the block's size into currentTextBlock (see wordFontsSize_).
  void resolveWordFont(uint8_t slot);
  // Effective fontId for a block: its heading font when set, else the body fontId.
  int effectiveFontId(const BlockStyle& bs) const { return bs.headingFontId != 0 ? bs.headingFontId : fontId; }
  // Line height for a block, honoring the taller heading font (residual multiplier on top)
  // or the body-font scale path. Centralizes the layout-time sizing. Defined in the .cpp
  // because it dereferences GfxRenderer, which is only forward-declared here.
  int effectiveLineHeight(const BlockStyle& bs) const;
  // See pageBlock_. wireTextBlock hands currentTextBlock the arena and the hook;
  // wireParagraphLines does the same for any text laid out as a paragraph of the page.
  void wireTextBlock();
  void wireParagraphLines(ParsedText& text);
  void beforeLineHook(uint8_t maxSizePct);
  // Widow/orphan control and keep-with-next (CSS defaults: orphans 2, widows 2), decided before
  // each line of currentTextBlock is placed: true when the page should end before a line that
  // still fits. A placed line is never moved, so the decision looks ahead instead. `nextLine`
  // asks the same for the line after the one about to be placed.
  bool keepTogetherBreak(int lineHeight, bool nextLine = false) const;
  static constexpr int kOrphans = 2;
  static constexpr int kWidows = 2;
  static constexpr int kKeepWithNextLines = 2;  // body lines a heading keeps room for below it
  // Lines of the paragraph being laid out already on the current page.
  uint16_t paragraphLinesOnPage_ = 0;
  void ensurePageBlock();
  void releasePageBlock();
};
