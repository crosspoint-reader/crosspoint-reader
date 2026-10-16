#pragma once

#include <MdParser.h>
#include <Txt.h>

#include <vector>

#include "CrossPointSettings.h"
#include "LineReaderActivity.h"

struct MdHeading {
  size_t offset = 0;
  int level = 1;
  std::string title;
  int pageIndex = -1;
};

class MdReaderActivity final : public LineReaderActivity {
  // Table-line flags on RenderedLine::tableFlags.
  static constexpr uint8_t TBL_TOP = 0x01;      // rule above: table (or page-continuation) top border
  static constexpr uint8_t TBL_ROW_END = 0x02;  // rule below: row separator / bottom border
  static constexpr uint8_t TBL_HEADER = 0x04;   // the rule below is the 2px header rule

  // Grid geometry, mirroring the EPUB table policy (lib/Epub/Epub/Page.h).
  static constexpr uint8_t MAX_TABLE_COLS = 8;
  static constexpr int TABLE_CELL_PAD_X = 5;
  static constexpr int MIN_COL_INNER_WIDTH = 24;

  // A single rendered line on screen (after word-wrapping)
  struct RenderedLine {
    std::vector<MdParser::Span> spans;
    // Table grid lines only: x of each span, relative to the content left edge
    // (alignment is baked in at layout time). Parallel to |spans|.
    std::vector<int16_t> spanX;
    int indent = 0;     // left indent in pixels
    bool isHR = false;  // draw as horizontal rule
    bool isCodeBlock = false;
    uint8_t tableCols = 0;   // >0: table grid line; drives the column rules
    uint8_t tableFlags = 0;  // TBL_* bits
  };

  // State of a pipe table left open at a page boundary, so a continuation page
  // lays its rows on the same grid. Packs into the page index cache: bits 0-3
  // column count (0 = no open table, COLS_FLATTENED = the open table is being
  // flattened), bits 4-19 per-column alignment (2 bits, MdParser::TableAlign).
  struct TableContext {
    // The grid cannot represent the table (too many / too narrow columns);
    // its rows keep the flattened "a | b | c" rendering until the table ends.
    static constexpr uint8_t COLS_FLATTENED = 0xF;

    uint8_t cols = 0;
    uint8_t aligns[MAX_TABLE_COLS] = {};  // MdParser::TableAlign values

    bool active() const { return cols != 0; }
    bool flattened() const { return cols == COLS_FLATTENED; }
    uint32_t pack() const;
    static TableContext unpack(uint32_t v);
  };

  std::vector<uint8_t> pageCodeBlockState;  // 1 if page starts inside a code block
  std::vector<uint32_t> pageTableState;     // packed TableContext at page start
  std::vector<RenderedLine> currentPageLines;
  std::vector<uint8_t> pageBuffer;

  std::vector<MdHeading> headings;
  int currentHeadingIndex = -1;

  // Indent constants (in pixels)
  static constexpr int LIST_INDENT = 20;
  static constexpr int BLOCKQUOTE_INDENT = 16;
  static constexpr int CODE_INDENT = 8;

  void renderPage();
  void renderStatusBar() const;

  void initializeReader();
  bool loadPageAtOffset(size_t offset, bool startInCodeBlock, TableContext startTable,
                        std::vector<RenderedLine>& outLines, size_t& nextOffset, bool& endInCodeBlock,
                        TableContext& endTable);
  void buildPageIndex();
  bool loadPageIndexCache();
  void savePageIndexCache() const;
  void scanHeadings();
  void assignHeadingPageNumbers();
  int getHeadingIndexForOffset(size_t offset) const;
  void jumpToHeading(bool next);

  // Word-wrap a parsed markdown line into one or more RenderedLines.
  // Returns true if all content was emitted, false if truncated by maxLines.
  bool wordWrapParsedLine(const MdParser::ParsedLine& parsed, int indent, std::vector<RenderedLine>& outLines,
                          int maxLines, bool isCodeBlock = false);

  // Core span wrapping shared by flowed lines and table cells: wrap |spans|
  // into visual lines no wider than availableWidth, appending each line's
  // spans and measured width. Returns false when stopped by maxLines.
  bool wrapSpans(const std::vector<MdParser::Span>& spans, int availableWidth, int maxLines,
                 std::vector<std::vector<MdParser::Span>>& outLines, std::vector<int>& outWidths) const;

  // Lay out one pipe-table row on the ctx grid. Returns false when the row
  // needs more than maxLines visual lines; the caller rolls it to the next
  // page, or flattens it when even an empty page cannot hold it.
  bool layoutTableRow(const MdParser::ParsedLine& parsed, const TableContext& ctx, bool isHeader, bool drawTopRule,
                      int maxLines, std::vector<RenderedLine>& outLines);

  // Fallback for tables the grid cannot represent: cells joined into one
  // flowed line, "a | b | c", bold when a header row.
  bool appendFlattenedTableRow(const MdParser::ParsedLine& parsed, bool isHeader, int maxLines,
                               std::vector<RenderedLine>& outLines);

  // Measure total pixel width of a span list
  int measureSpans(const std::vector<MdParser::Span>& spans) const;

  // Draw one span at (x, y) including its underline/strikethrough decoration;
  // returns the span's advance width.
  int drawSpanAt(int x, int y, const MdParser::Span& span) const;

 protected:
  void onReaderExit() override;
  // Opens the ToC overlay when the document has headings.
  bool onConfirmShortPress() override;
  // Keeps currentHeadingIndex in sync with the page shown.
  void onPageChanged() override;

 public:
  explicit MdReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt)
      : LineReaderActivity("MdReader", "MDR", renderer, mappedInput, std::move(txt)) {}
  void render(RenderLock&&) override;
  void onButtonAction(CrossPointSettings::BUTTON_ACTION action) override;
};
