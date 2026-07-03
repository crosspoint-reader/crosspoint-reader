#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "ParsedText.h"

class GfxRenderer;
class PageElement;

// Narrow interface the chapter parser implements so table layout can place
// elements on pages (and break pages) without seeing parser internals.
// TableLayout only ever holds a sink by reference for the duration of a call,
// never owns one, so the destructor is protected and non-virtual.
class TablePageSink {
 public:
  virtual int16_t currentY() const = 0;
  virtual int16_t pageHeight() const = 0;
  // Flush the current page (if it has content) and start a fresh one at y = 0.
  virtual void completePage() = 0;
  virtual void addElement(std::shared_ptr<PageElement> element) = 0;
  virtual void advanceY(int16_t dy) = 0;

 protected:
  ~TablePageSink() = default;
};

// Lays out <table> content as a real grid with drawn cell borders.
//
// Nothing is ever truncated: content that does not fit the grid changes HOW it
// renders, never WHETHER it renders. Oversized rows and overflow columns fall
// back to the stacked form (full-width paragraphs with separator rules), and
// cells that grow past CELL_STREAM_THRESHOLD words stream their finished lines
// to pages while parsing continues, so even a chapter-sized cell renders in
// bounded memory.
//
// Memory strategy (the ~380KB heap is the hard constraint): only a small
// sample of leading rows is buffered to derive column widths; after that every
// row flushes as its </tr> closes. Grid rows must materialize all their lines
// at once (the row height is needed for the borders), so rows heavier than
// GRID_ROW_WORD_LIMIT render stacked instead — bounding the per-row transient
// to roughly 60KB worst case.
//
// When the table has more columns than the viewport can legibly hold (or is
// single-column), the whole table uses the stacked layout.
class TableLayout {
 public:
  static constexpr size_t MAX_COLS = 8;
  // Once an open cell holds this many words the parser must call
  // streamOpenCell so the cell's leading lines flush to pages instead of
  // accumulating on the heap.
  static constexpr size_t CELL_STREAM_THRESHOLD = 320;

  TableLayout(const GfxRenderer& renderer, int fontId, float lineCompression, int16_t originX, uint16_t availableWidth);

  // Presentation policy for a cell's text block: th centered+indent-free,
  // td left-aligned, CSS text-align/dir honored when present.
  static BlockStyle cellBlockStyle(bool isHeaderCell, const CssStyle& cssStyle);

  void startRow();
  // Returns false only when a cell is already open (defensive; the parser's
  // inTableCell gate prevents it). Never drops content otherwise.
  bool startCell(uint8_t colSpan, TablePageSink& sink);
  // Hands ownership of the finished cell's words to the table.
  void endCell(std::unique_ptr<ParsedText> text, TablePageSink& sink);
  void endRow(TablePageSink& sink);
  // Called on </table>: flushes anything still buffered and closes the grid.
  void finish(TablePageSink& sink);
  // Flushes the finished lines of the still-open cell to pages, keeping only
  // the trailing partial line; the parser keeps appending words to `text`
  // afterwards. Forces stacked rendering for the affected scope (a streamed
  // cell can never be part of a materialized grid row).
  void streamOpenCell(ParsedText& text, TablePageSink& sink);

 private:
  static constexpr int16_t BORDER = 1;
  static constexpr int16_t CELL_PAD_X = 4;
  static constexpr int16_t CELL_PAD_Y = 3;
  // Column widths are derived from the first few rows only, so an arbitrarily
  // long table never buffers more than this sample.
  static constexpr size_t SAMPLE_ROW_LIMIT = 6;
  static constexpr size_t SAMPLE_WORD_LIMIT = 600;
  // A grid row's lines exist together in RAM during emitRowColumns (each line
  // is a TextBlock of ~200 bytes); heavier rows render stacked instead.
  static constexpr size_t GRID_ROW_WORD_LIMIT = CELL_STREAM_THRESHOLD;

  // Sampling: leading rows buffer until column layout can be decided.
  // Grid: rows lay out as bordered columns (per-row stacked degradation for
  // rows that are too heavy or too wide). Stacked: every cell renders as a
  // full-width paragraph the moment it closes.
  enum class Mode : uint8_t { Sampling, Grid, Stacked };

  struct Cell {
    std::unique_ptr<ParsedText> text;
    uint8_t colSpan = 1;
  };
  using Row = std::vector<Cell>;

  const GfxRenderer& renderer;
  const int fontId;
  const int16_t lineHeight;
  const int16_t originX;
  const uint16_t availableWidth;

  Mode mode = Mode::Sampling;
  std::vector<Row> bufferedRows;
  Row currentRow;
  bool rowOpen = false;
  bool cellOpen = false;
  bool openCellStreamed = false;   // open cell's leading lines already on pages
  bool currentRowStacked = false;  // grid mode: this row degraded to stacked
  bool stackedRowHadContent = false;
  uint8_t pendingColSpan = 1;
  size_t currentRowCols = 0;
  size_t currentRowWords = 0;
  size_t sampledWords = 0;

  std::vector<uint16_t> columnWidths;
  std::vector<int16_t> boundaryXs;  // x of each vertical border line, size numCols + 1
  int16_t tableX = 0;
  uint16_t totalWidth = 0;
  bool topBorderPending = true;
  bool emittedAnything = false;

  // Per-row scratch, reused across rows to avoid per-row heap churn
  // (capacity is retained by clear()).
  std::vector<std::vector<std::shared_ptr<TextBlock>>> rowCellLines;
  std::vector<int16_t> rowCellX;
  std::vector<bool> rowBoundarySkipped;
  std::vector<int16_t> rowBoundaries;

  void decideLayout();
  // Abandons sampling for the whole table (content unsuited to a grid):
  // emits everything buffered so far in stacked form and streams from then on.
  void switchToStacked(TablePageSink& sink);
  // Degrades the in-progress row to stacked form: emits its buffered cells
  // immediately; subsequent cells of the row emit as they close.
  void convertCurrentRowToStacked(TablePageSink& sink);
  void flushBufferedRows(TablePageSink& sink);
  void emitRow(Row& row, TablePageSink& sink);
  // Lays every cell of the row out into rowCellLines/rowCellX and fills
  // rowBoundaries; returns the row's line count (>= 1).
  size_t layoutRowCells(Row& row);
  void emitRowColumns(Row& row, TablePageSink& sink);
  void emitRowStacked(Row& row, TablePageSink& sink);
  // Lays `text` out at full table width and emits the lines (the stacked
  // building block, also used for streamed-cell chunks).
  void emitStackedCell(ParsedText& text, TablePageSink& sink, bool includeLastLine);
  void emitStackedRowSeparator(TablePageSink& sink);
  void addRect(TablePageSink& sink, int16_t x, int16_t y, uint16_t w, uint16_t h) const;
};
