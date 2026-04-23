#pragma once

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

class ChapterHtmlSlimParser {
  std::shared_ptr<Epub> epub;
  const std::string& filepath;
  GfxRenderer& renderer;
  std::function<void(std::unique_ptr<Page>)> completePageFn;
  std::function<void(int)> progressFn;  // Progress callback (0-100)
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  int skipTextUntilDepth = INT_MAX;  // skip character data inside synthetic zero-height spacer <p>
  int boldUntilDepth = INT_MAX;
  int italicUntilDepth = INT_MAX;
  int underlineUntilDepth = INT_MAX;
  int preUntilDepth = INT_MAX;  // set when inside a <pre> element; enables \n → line-break handling
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

  // Style tracking (replaces depth-based approach)
  struct StyleStackEntry {
    int depth = 0;
    bool hasBold = false, bold = false;
    bool hasItalic = false, italic = false;
    bool hasUnderline = false, underline = false;
  };
  std::vector<StyleStackEntry> inlineStyleStack;
  CssStyle currentCssStyle;
  bool effectiveBold = false;
  bool effectiveItalic = false;
  bool effectiveUnderline = false;
  int tableDepth = 0;
  int tableRowIndex = 0;
  int tableColIndex = 0;

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

  // Paragraph index tracking for XPath-to-page lookup table.
  // Counts <p> sibling indices (1-based, matching XPath convention) during page building.
  // Stored per page in the section cache so that XPath p[N] can be resolved to a page
  // without reparsing, and current page can generate an XPath without reparsing.
  uint16_t xpathParagraphIndex = 0;  // current <p> sibling index (1-based)
  int xpathBodyDepth = -1;           // depth of the <body> element (-1 = not yet seen)
  // Byte offset of the most recent direct-body-child element start (any tag at xpathBodyDepth+1).
  // Recorded at the same depth condition that increments xpathParagraphIndex, so the stored
  // offset is guaranteed to land on a body-child element boundary. This keeps the XPath forward
  // mapper's partial-parse heuristic reliable for wrapped chapters: without this, the offset
  // could point mid-way into a nested <div>/<section>, which confuses partialBaseDepth.
  uint32_t lastBodyChildByteOffset = 0;

  struct ParagraphLutEntry {
    uint32_t xhtmlByteOffset;  // byte offset of most recent body-child element start at page break
    uint16_t paragraphIndex;   // 1-based <p> index at page completion
  };
  std::vector<ParagraphLutEntry> paragraphLutPerPage;  // deep LUT: one entry per page

  // Active parser handle during parseAndBuildPages(), nullptr otherwise.
  // Stored as a member so page-break sites (addLineToPage, image breaks) can call
  // XML_GetCurrentByteIndex without needing the parser threaded through every call.
  XML_Parser activeParser = nullptr;

  // Footnote link tracking
  bool insideFootnoteLink = false;
  int footnoteLinkDepth = -1;
  FootnoteEntry currentFootnote = {};
  int currentFootnoteLinkTextLen = 0;
  std::vector<std::pair<int, FootnoteEntry>> pendingFootnotes;  // <wordIndex, entry>
  int wordsExtractedInBlock = 0;

  // Per-chapter caches: resolveStyle and parseInlineStyle are called for every HTML element;
  // caching by (tag|classAttr) and styleAttr avoids repeated string operations and hash lookups.
  std::unordered_map<std::string, CssStyle> cssStyleCache_;
  std::unordered_map<std::string, CssStyle> inlineStyleCache_;

  void updateEffectiveInlineStyle();
  void startNewTextBlock(const BlockStyle& blockStyle);
  void flushPartWordBuffer();
  void makePages();
  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL defaultHandlerExpand(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

 public:
  explicit ChapterHtmlSlimParser(std::shared_ptr<Epub> epub, const std::string& filepath, GfxRenderer& renderer,
                                 const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                 const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                 const uint16_t viewportHeight, const bool hyphenationEnabled,
                                 const std::function<void(std::unique_ptr<Page>)>& completePageFn,
                                 const bool embeddedStyle, const std::string& contentBase,
                                 const std::string& imageBasePath, const uint8_t imageRendering = 0,
                                 std::vector<std::string> tocAnchors = {},
                                 const std::function<void(int)>& progressFn = nullptr,
                                 const CssParser* cssParser = nullptr)

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
        completePageFn(completePageFn),
        progressFn(progressFn),
        cssParser(cssParser),
        embeddedStyle(embeddedStyle),
        imageRendering(imageRendering),
        contentBase(contentBase),
        imageBasePath(imageBasePath),
        tocAnchors(std::move(tocAnchors)) {}

  ~ChapterHtmlSlimParser() = default;
  bool parseAndBuildPages();
  ParsedText::LineProcessResult addLineToPage(std::shared_ptr<TextBlock> line, bool lineEndsWithHyphenatedWord,
                                              bool suppressHyphenationRetry);
  const std::vector<std::pair<std::string, uint16_t>>& getAnchors() const { return anchorData; }
  const std::vector<ParagraphLutEntry>& getParagraphLutPerPage() const { return paragraphLutPerPage; }
};
