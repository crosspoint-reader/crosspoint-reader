#pragma once

#include <Print.h>
#include <expat.h>

#include <climits>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../FootnoteEntry.h"
#include "../ParsedText.h"
#include "../blocks/ImageBlock.h"
#include "../blocks/TextBlock.h"
#include "../css/CssParser.h"
#include "../css/CssStyle.h"

class Page;
class GfxRenderer;
class Epub;

#define MAX_WORD_SIZE 200

class ChapterHtmlSlimParser final : public Print {
  std::shared_ptr<Epub> epub;
  GfxRenderer& renderer;
  std::function<void(std::unique_ptr<Page>)> completePageFn;
  std::function<void(int)> progressFn;  // Progress callback (0-100)
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  int skipTextUntilDepth = INT_MAX;  // skip character data inside synthetic zero-height spacer <p>
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
  bool nextWordContinues = false;  // true when next flushed word attaches to previous (inline element boundary)
  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  std::unique_ptr<Page> currentPage = nullptr;
  int16_t currentPageNextY = 0;
  int fontId;
  float lineCompression;
  bool extraParagraphSpacing;
  uint8_t paragraphAlignment;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  bool hyphenationEnabled;
  const CssParser* cssParser;
  bool embeddedStyle;
  uint8_t imageRendering;
  std::string contentBase;
  std::string imageBasePath;
  int imageCounter = 0;
  bool lowMemoryImageFallback = false;

  // Style tracking (replaces depth-based approach)
  struct StyleStackEntry {
    int depth = 0;
    bool hasBold = false, bold = false;
    bool hasItalic = false, italic = false;
    bool hasUnderline = false, underline = false;
    bool hasStrikethrough = false, strikethrough = false;
    bool hasSup = false, sup = false;
    bool hasSub = false, sub = false;
  };
  std::vector<StyleStackEntry> inlineStyleStack;
  CssStyle currentCssStyle;
  bool effectiveBold = false;
  bool effectiveItalic = false;
  bool effectiveUnderline = false;
  bool effectiveStrikethrough = false;
  bool effectiveSup = false;
  bool effectiveSub = false;
  // Buffered table model — populated while inside <table>, emitted on </table>
  struct BufferedTableCell {
    std::unique_ptr<ParsedText> text;
    bool isHeader = false;
  };
  struct BufferedTableRow {
    std::vector<BufferedTableCell> cells;
    bool isHeaderRow = false;  // true when all cells in this row are <th>
  };
  struct BufferedTable {
    std::vector<BufferedTableRow> rows;
    int depth = 0;             // nesting depth; > 1 means we're inside a nested table
    bool unsupported = false;  // true → emit as paragraphs instead of grid
  };
  std::unique_ptr<BufferedTable> currentTable;
  BufferedTableCell* currentTableCell = nullptr;  // non-null while inside <td>/<th>

  struct ListEntry {
    int depth;
    bool isOrdered;
    int counter;
  };
  std::vector<ListEntry> listStack;

  // Anchor-to-page mapping: tracks which page each HTML id attribute lands on
  int completedPageCount = 0;
  std::vector<std::pair<std::string, uint16_t>> anchorData;
  std::string pendingAnchorId;  // deferred until after previous text block is flushed
  std::vector<std::string> tocAnchors;

  // External printed-page labels sourced from NCX <pageList> or EPUB 3 nav page-list.
  // Keyed by HTML id (anchor fragment). When the parser encounters an element whose id
  // matches one of these, it records the label as if the element were an inline
  // doc-pagebreak marker. Anchors already labeled this way are not re-recorded if the
  // same element also carries an inline pagebreak attribute.
  std::vector<std::pair<std::string, std::string>> externalPageBreakAnchors;
  // Optional label for the start of this XHTML file (NCX entries with no fragment).
  std::string topOfFilePageLabel;
  bool topOfFilePageLabelEmitted = false;

  // Page break label mapping: stores the printed page label from EPUB pagebreak markers
  // and the section page index where that printed page begins.
  std::vector<std::pair<uint16_t, std::string>> pageBreakLabels;

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
  // Byte offset of the most recent direct-body-child element start (any tag at xpathBodyDepth+1).
  // Recorded at the same depth condition that increments xpathParagraphIndex, so the stored
  // offset is guaranteed to land on a body-child element boundary. This keeps the XPath forward
  // mapper's partial-parse heuristic reliable for wrapped chapters: without this, the offset
  // could point mid-way into a nested <div>/<section>, which confuses partialBaseDepth.
  uint32_t lastBodyChildByteOffset = 0;

  struct ParagraphLutEntry {
    uint32_t xhtmlByteOffset;  // byte offset of most recent body-child element start at page break
    uint16_t paragraphIndex;   // 1-based <p> index at page completion
    uint16_t listItemIndex;    // running <li> count at page completion (any depth)
  };
  std::vector<ParagraphLutEntry> paragraphLutPerPage;  // deep LUT: one entry per page

  // Active parser handle during streaming, nullptr otherwise.
  // Stored as a member so page-break sites (addLineToPage, image breaks) can call
  // XML_GetCurrentByteIndex without needing the parser threaded through every call.
  XML_Parser activeParser = nullptr;

  // Streaming state for the Print-derived parsing API.
  size_t totalStreamSize = 0;
  size_t bytesStreamed = 0;
  int lastReportedProgress = -1;
  int progressStepPercent = 0;
  bool progressUiEnabled = true;
  bool streamFailed = false;
  uint32_t streamStartTimeMs = 0;

  // Footnote link tracking
  bool insideFootnoteLink = false;
  int footnoteLinkDepth = -1;
  FootnoteEntry currentFootnote = {};
  int currentFootnoteLinkTextLen = 0;
  std::vector<std::pair<int, FootnoteEntry>> pendingFootnotes;  // <wordIndex, entry>
  int wordsExtractedInBlock = 0;
  bool bionicReadingEnabled = false;
  bool layoutFailed = false;

  // Per-chapter caches: resolveStyle and parseInlineStyle are called for every HTML element;
  // caching by (tag|classAttr) and styleAttr avoids repeated string operations and hash lookups.
  std::unordered_map<std::string, CssStyle> cssStyleCache_;
  std::unordered_map<std::string, CssStyle> inlineStyleCache_;

  void updateEffectiveInlineStyle();
  bool ensureHeapForTextLayout(const char* phase);
  void startNewTextBlock(const BlockStyle& blockStyle);
  bool flushPartWordBuffer();
  void makePages();
  void emitBufferedTable();
  void emitTableAsFragments(BufferedTable& table);
  void emitTableAsParagraphs(BufferedTable& table);
  // Emit currentPage to the consumer while keeping paragraphLutPerPage and completedPageCount
  // in lockstep. Every page break MUST go through this helper; open-coded completePageFn
  // calls risk desynchronising paragraphLutPerPage and failing the size check in Section.cpp.
  void emitPage(uint32_t xhtmlByteOffset);
  void recordPageBreakLabel(const std::string& label);
  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL defaultHandlerExpand(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

 public:
  explicit ChapterHtmlSlimParser(
      std::shared_ptr<Epub> epub, GfxRenderer& renderer, const int fontId, const float lineCompression,
      const bool extraParagraphSpacing, const uint8_t paragraphAlignment, const uint16_t viewportWidth,
      const uint16_t viewportHeight, const bool hyphenationEnabled, const bool bionicReadingEnabled,
      const std::function<void(std::unique_ptr<Page>)>& completePageFn, const bool embeddedStyle,
      const std::string& contentBase, const std::string& imageBasePath, const uint8_t imageRendering = 0,
      std::vector<std::string> tocAnchors = {}, const std::function<void(int)>& progressFn = nullptr,
      const CssParser* cssParser = nullptr)

      : epub(epub),
        renderer(renderer),
        fontId(fontId),
        lineCompression(lineCompression),
        extraParagraphSpacing(extraParagraphSpacing),
        paragraphAlignment(paragraphAlignment),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight),
        hyphenationEnabled(hyphenationEnabled),
        bionicReadingEnabled(bionicReadingEnabled),
        completePageFn(completePageFn),
        progressFn(progressFn),
        cssParser(cssParser),
        embeddedStyle(embeddedStyle),
        imageRendering(imageRendering),
        contentBase(contentBase),
        imageBasePath(imageBasePath),
        tocAnchors(std::move(tocAnchors)) {}

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

  // Print interface — fed by Epub::readItemContentsToStream.
  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;

  ParsedText::LineProcessResult addLineToPage(std::shared_ptr<TextBlock> line, bool lineEndsWithHyphenatedWord,
                                              bool suppressHyphenationRetry);
  const std::vector<std::pair<std::string, uint16_t>>& getAnchors() const { return anchorData; }
  const std::vector<std::pair<uint16_t, std::string>>& getPageBreakLabels() const { return pageBreakLabels; }
  const std::vector<ParagraphLutEntry>& getParagraphLutPerPage() const { return paragraphLutPerPage; }

  // Supplies printed-page labels from NCX <pageList> for this chapter. `anchors` maps
  // HTML id -> label; an entry with an empty id applies to the first page of this file.
  void setExternalPageBreakAnchors(std::vector<std::pair<std::string, std::string>> anchors);
};
