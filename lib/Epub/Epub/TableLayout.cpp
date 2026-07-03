#include "TableLayout.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <algorithm>
#include <new>

#include "Page.h"

TableLayout::TableLayout(const GfxRenderer& renderer, const int fontId, const float lineCompression,
                         const int16_t originX, const uint16_t availableWidth)
    : renderer(renderer),
      fontId(fontId),
      lineHeight(static_cast<int16_t>(renderer.getLineHeight(fontId) * lineCompression + 0.5f)),
      originX(originX),
      availableWidth(availableWidth) {
  bufferedRows.reserve(SAMPLE_ROW_LIMIT);
}

BlockStyle TableLayout::cellBlockStyle(const bool isHeaderCell, const CssStyle& cssStyle) {
  BlockStyle style;
  style.textAlignDefined = true;
  style.alignment =
      cssStyle.hasTextAlign() ? cssStyle.textAlign : (isHeaderCell ? CssTextAlign::Center : CssTextAlign::Left);
  // Suppress the paragraph first-line indent inside cells.
  style.textIndentDefined = true;
  style.textIndent = 0;
  if (cssStyle.hasDirection()) {
    style.directionDefined = true;
    style.isRtl = cssStyle.direction == CssTextDirection::Rtl;
  }
  return style;
}

void TableLayout::startRow() {
  rowOpen = true;
  cellOpen = false;
  openCellStreamed = false;
  currentRowStacked = false;
  stackedRowHadContent = false;
  currentRowCols = 0;
  currentRowWords = 0;
  currentRow.clear();
  currentRow.reserve(MAX_COLS);
}

bool TableLayout::startCell(const uint8_t colSpan, TablePageSink& sink) {
  if (!rowOpen) {
    // Malformed HTML (<td> without <tr>): open an implicit row instead of losing content.
    startRow();
  }
  if (cellOpen) {
    return false;  // unreachable via the parser's inTableCell gate; defensive
  }

  // A row wider than the grid can hold never loses cells; the layout degrades.
  if (mode == Mode::Sampling && currentRowCols >= MAX_COLS) {
    LOG_DBG("TBL", "Row exceeds %u columns, rendering table stacked", static_cast<unsigned>(MAX_COLS));
    switchToStacked(sink);
  } else if (mode == Mode::Grid && !currentRowStacked && currentRowCols >= columnWidths.size()) {
    LOG_DBG("TBL", "Row wider than sampled %u columns, rendering row stacked",
            static_cast<unsigned>(columnWidths.size()));
    convertCurrentRowToStacked(sink);
  }

  cellOpen = true;
  openCellStreamed = false;
  const size_t colsLeft = currentRowCols < MAX_COLS ? MAX_COLS - currentRowCols : 1;
  pendingColSpan = static_cast<uint8_t>(std::clamp<size_t>(colSpan, 1, colsLeft));
  return true;
}

void TableLayout::endCell(std::unique_ptr<ParsedText> text, TablePageSink& sink) {
  if (!cellOpen) {
    return;  // defensive: endCell without startCell
  }
  cellOpen = false;
  currentRowCols += pendingColSpan;
  const size_t words = text ? text->size() : 0;
  const bool streamedTail = openCellStreamed;
  openCellStreamed = false;

  if (mode == Mode::Sampling) {
    // streamOpenCell forces Stacked before any streaming, so a sampled cell is
    // always complete here.
    sampledWords += words;
    Cell cell;
    cell.colSpan = pendingColSpan;
    cell.text = std::move(text);
    currentRow.push_back(std::move(cell));

    // Bound the sample buffer mid-row as well as at row ends: without this, a
    // single wide row of heavy cells could buffer far past the sample limit.
    if (sampledWords >= SAMPLE_WORD_LIMIT) {
      decideLayout();
      flushBufferedRows(sink);
      if (mode == Mode::Stacked) {
        convertCurrentRowToStacked(sink);
      } else {
        currentRowWords = 0;
        for (const auto& cell : currentRow) {
          currentRowWords += cell.text ? cell.text->size() : 0;
        }
        // The partial row can be heavier or wider than the grid decided from
        // the complete rows; degrade it to stacked rather than losing cells.
        if (currentRowWords > GRID_ROW_WORD_LIMIT || currentRowCols > columnWidths.size()) {
          convertCurrentRowToStacked(sink);
        }
      }
    }
    return;
  }

  if (mode == Mode::Grid && !currentRowStacked) {
    // A row about to exceed the grid budget renders stacked (grid resumes with
    // the next row); its buffered cells emit now and lose nothing.
    if (!streamedTail && currentRowWords + words > GRID_ROW_WORD_LIMIT) {
      LOG_DBG("TBL", "Row exceeds %u-word grid budget, rendering row stacked",
              static_cast<unsigned>(GRID_ROW_WORD_LIMIT));
      convertCurrentRowToStacked(sink);
    } else if (!streamedTail) {
      currentRowWords += words;
      Cell cell;
      cell.colSpan = pendingColSpan;
      cell.text = std::move(text);
      currentRow.push_back(std::move(cell));
      return;
    }
  }

  // Stacked table, degraded row, or the tail of a streamed cell: emit now.
  if (text && !text->isEmpty()) {
    emitStackedCell(*text, sink, true);
  }
}

void TableLayout::endRow(TablePageSink& sink) {
  cellOpen = false;
  rowOpen = false;
  openCellStreamed = false;

  if (mode == Mode::Sampling) {
    if (!currentRow.empty()) {
      bufferedRows.push_back(std::move(currentRow));
      currentRow.clear();
    }
    if (bufferedRows.size() >= SAMPLE_ROW_LIMIT || sampledWords >= SAMPLE_WORD_LIMIT) {
      decideLayout();
      flushBufferedRows(sink);
    }
    return;
  }

  if (mode == Mode::Grid && !currentRowStacked) {
    if (!currentRow.empty()) {
      emitRow(currentRow, sink);
      currentRow.clear();
    }
    currentRowWords = 0;
    return;
  }

  // Stacked table or degraded grid row: cells already emitted as they closed.
  if (stackedRowHadContent) {
    emitStackedRowSeparator(sink);
  }
  stackedRowHadContent = false;
  currentRowStacked = false;
  currentRowWords = 0;
}

void TableLayout::finish(TablePageSink& sink) {
  // Salvage a row whose </tr> never arrived.
  endRow(sink);
  if (mode == Mode::Sampling) {
    decideLayout();
    flushBufferedRows(sink);
  }
  if (emittedAnything) {
    // Breathing room below the table, mirroring the gap added above it.
    if (sink.currentY() > 0 && sink.currentY() + lineHeight / 2 <= sink.pageHeight()) {
      sink.advanceY(lineHeight / 2);
    }
  }
}

void TableLayout::streamOpenCell(ParsedText& text, TablePageSink& sink) {
  if (!cellOpen) {
    return;
  }
  if (!openCellStreamed) {
    // A streamed cell can never join a materialized grid row: degrade the
    // affected scope to stacked BEFORE the cell's first lines hit the page,
    // so everything renders in document order.
    if (mode == Mode::Sampling) {
      LOG_DBG("TBL", "Cell exceeds %u words, rendering table stacked", static_cast<unsigned>(CELL_STREAM_THRESHOLD));
      switchToStacked(sink);
    } else if (mode == Mode::Grid && !currentRowStacked) {
      LOG_DBG("TBL", "Cell exceeds %u words, rendering row stacked", static_cast<unsigned>(CELL_STREAM_THRESHOLD));
      convertCurrentRowToStacked(sink);
    }
    openCellStreamed = true;
  }
  // Flush all finished lines; the trailing partial line's words stay in `text`
  // and keep collecting.
  emitStackedCell(text, sink, false);
}

void TableLayout::switchToStacked(TablePageSink& sink) {
  mode = Mode::Stacked;
  flushBufferedRows(sink);
  convertCurrentRowToStacked(sink);
}

void TableLayout::convertCurrentRowToStacked(TablePageSink& sink) {
  currentRowStacked = true;
  currentRowWords = 0;
  for (auto& cell : currentRow) {
    if (cell.text && !cell.text->isEmpty()) {
      emitStackedCell(*cell.text, sink, true);
    }
    cell.text.reset();
  }
  currentRow.clear();
}

void TableLayout::decideLayout() {
  size_t numCols = 0;
  for (const auto& row : bufferedRows) {
    size_t cols = 0;
    for (const auto& cell : row) {
      cols += cell.colSpan;
    }
    numCols = std::max(numCols, cols);
  }
  numCols = std::min(numCols, MAX_COLS);

  // Narrowest legible column: roughly two characters of the current font.
  const int minColWidth = std::max(8, renderer.getFontAscenderSize(fontId) * 2);
  const int slotOverhead = 2 * CELL_PAD_X + BORDER;
  const size_t maxFit = availableWidth > static_cast<uint16_t>(BORDER + minColWidth + slotOverhead)
                            ? static_cast<size_t>((availableWidth - BORDER) / (minColWidth + slotOverhead))
                            : 1;
  // A 1-column grid is just a boxed paragraph; render it (and anything too wide) stacked.
  if (numCols < 2 || numCols > maxFit) {
    if (numCols > maxFit) {
      LOG_DBG("TBL", "Table with %u columns too wide for viewport (%u fit), using stacked layout",
              static_cast<unsigned>(numCols), static_cast<unsigned>(maxFit));
    }
    mode = Mode::Stacked;
    return;
  }
  mode = Mode::Grid;

  // Natural (single-line) width of each column, sampled from the buffered rows.
  // Spanning cells contribute an even share to each spanned column.
  std::vector<int32_t> natural(numCols, 0);
  for (auto& row : bufferedRows) {
    size_t col = 0;
    for (auto& cell : row) {
      if (col >= numCols) break;
      const size_t span = std::min<size_t>(cell.colSpan, numCols - col);
      if (cell.text && !cell.text->isEmpty()) {
        int naturalWidth = 0;
        int maxWordWidth = 0;
        cell.text->measureIntrinsicWidths(renderer, fontId, naturalWidth, maxWordWidth);
        const auto perCol = static_cast<int32_t>(naturalWidth / static_cast<int>(span));
        for (size_t c = col; c < col + span; ++c) {
          natural[c] = std::max(natural[c], perCol);
        }
      }
      col += span;
    }
  }

  // Distribute the available content width: every column first gets
  // min(natural, fair share); leftover space goes proportionally to columns
  // whose content wants more. Fully satisfied content leaves the table
  // narrower than the viewport (content-fit).
  const auto avail = static_cast<int32_t>(availableWidth) - static_cast<int32_t>(numCols + 1) * BORDER -
                     static_cast<int32_t>(numCols) * 2 * CELL_PAD_X;
  const int32_t fair = avail / static_cast<int32_t>(numCols);
  columnWidths.assign(numCols, 0);
  int32_t unmetTotal = 0;
  for (size_t c = 0; c < numCols; ++c) {
    const int32_t want = std::max<int32_t>(natural[c], minColWidth);
    const int32_t base = std::min(want, fair);
    columnWidths[c] = static_cast<uint16_t>(std::max<int32_t>(base, 1));
    if (want > base) {
      unmetTotal += want - base;
    }
  }
  int32_t leftover = avail;
  for (const auto w : columnWidths) leftover -= w;
  if (leftover > 0 && unmetTotal > 0) {
    for (size_t c = 0; c < numCols; ++c) {
      const int32_t want = std::max<int32_t>(natural[c], minColWidth);
      const int32_t unmet = want - columnWidths[c];
      if (unmet > 0) {
        const auto extra = static_cast<int32_t>(static_cast<int64_t>(leftover) * unmet / unmetTotal);
        columnWidths[c] = static_cast<uint16_t>(columnWidths[c] + std::min(unmet, extra));
      }
    }
  }

  totalWidth = static_cast<uint16_t>((numCols + 1) * BORDER);
  for (const auto w : columnWidths) {
    totalWidth = static_cast<uint16_t>(totalWidth + w + 2 * CELL_PAD_X);
  }
  tableX = static_cast<int16_t>(originX + std::max(0, (availableWidth - totalWidth) / 2));

  boundaryXs.clear();
  boundaryXs.reserve(numCols + 1);
  int16_t x = tableX;
  for (size_t b = 0; b <= numCols; ++b) {
    boundaryXs.push_back(x);
    if (b < numCols) {
      x = static_cast<int16_t>(x + BORDER + columnWidths[b] + 2 * CELL_PAD_X);
    }
  }
}

void TableLayout::flushBufferedRows(TablePageSink& sink) {
  for (auto& row : bufferedRows) {
    emitRow(row, sink);
  }
  bufferedRows.clear();
}

void TableLayout::emitRow(Row& row, TablePageSink& sink) {
  if (mode == Mode::Grid) {
    // Sampled rows were buffered before the budget could apply to them; rows
    // too heavy to materialize as grid lines render stacked, in full.
    size_t words = 0;
    for (const auto& cell : row) {
      words += cell.text ? cell.text->size() : 0;
    }
    if (words <= GRID_ROW_WORD_LIMIT) {
      emitRowColumns(row, sink);
      return;
    }
    LOG_DBG("TBL", "Sampled row exceeds %u-word grid budget, rendering row stacked",
            static_cast<unsigned>(GRID_ROW_WORD_LIMIT));
  }
  emitRowStacked(row, sink);
}

void TableLayout::addRect(TablePageSink& sink, const int16_t x, const int16_t y, const uint16_t w,
                          const uint16_t h) const {
  if (w == 0 || h == 0) {
    return;
  }
  auto rect = std::shared_ptr<PageRect>(new (std::nothrow) PageRect(w, h, x, y));
  if (!rect) {
    LOG_ERR("TBL", "OOM: PageRect");
    return;
  }
  sink.addElement(std::move(rect));
}

size_t TableLayout::layoutRowCells(Row& row) {
  const size_t numCols = columnWidths.size();

  rowCellLines.clear();
  rowCellX.clear();
  rowBoundaries.clear();
  rowCellLines.reserve(row.size());
  rowCellX.reserve(row.size());
  // Border lines running through a colspan cell are suppressed.
  rowBoundarySkipped.assign(numCols + 1, false);

  size_t col = 0;
  size_t rowLines = 1;  // empty rows still get one line of height
  for (auto& cell : row) {
    if (col >= numCols) break;
    const size_t span = std::min<size_t>(std::max<uint8_t>(cell.colSpan, 1), numCols - col);
    int contentWidth = -2 * CELL_PAD_X;
    for (size_t c = col; c < col + span; ++c) {
      contentWidth += columnWidths[c] + 2 * CELL_PAD_X;
      if (c > col) contentWidth += BORDER;
    }
    if (contentWidth < 1) contentWidth = 1;
    for (size_t b = col + 1; b < col + span; ++b) {
      rowBoundarySkipped[b] = true;
    }

    rowCellX.push_back(static_cast<int16_t>(boundaryXs[col] + BORDER + CELL_PAD_X));
    rowCellLines.emplace_back();
    auto& lines = rowCellLines.back();
    if (cell.text && !cell.text->isEmpty()) {
      lines.reserve(cell.text->size());
      cell.text->layoutAndExtractLines(renderer, fontId, static_cast<uint16_t>(contentWidth),
                                       [&lines](const std::shared_ptr<TextBlock>& line) { lines.push_back(line); });
    }
    cell.text.reset();  // words are now held by the lines; free the source early
    rowLines = std::max(rowLines, lines.size());
    col += span;
  }

  // Vertical border x positions relevant for this row.
  rowBoundaries.reserve(numCols + 1);
  for (size_t b = 0; b <= numCols; ++b) {
    if (!rowBoundarySkipped[b]) {
      rowBoundaries.push_back(boundaryXs[b]);
    }
  }

  return rowLines;
}

void TableLayout::emitRowColumns(Row& row, TablePageSink& sink) {
  const int16_t pageH = sink.pageHeight();
  const size_t rowLines = layoutRowCells(row);

  // Pre-flight: make sure the row can open on this page with at least one text
  // line plus its closing padding and border; otherwise start a fresh page.
  if (topBorderPending && sink.currentY() > 0 && sink.currentY() + lineHeight / 2 <= pageH) {
    sink.advanceY(lineHeight / 2);  // breathing room above the table
  }
  {
    const int16_t y = sink.currentY();
    const auto opening =
        static_cast<int16_t>((topBorderPending ? BORDER : 0) + CELL_PAD_Y + lineHeight + CELL_PAD_Y + BORDER);
    if (y > 0 && y + opening > pageH) {
      sink.completePage();
    }
  }

  size_t emitted = 0;
  bool firstSegment = true;
  while (true) {
    if (topBorderPending) {
      addRect(sink, tableX, sink.currentY(), totalWidth, BORDER);
      sink.advanceY(BORDER);
      topBorderPending = false;
    }
    const int16_t segTop = sink.currentY();
    const int16_t contentY = static_cast<int16_t>(segTop + (firstSegment ? CELL_PAD_Y : 0));
    const size_t remaining = rowLines - emitted;
    const int spaceNoClose = pageH - contentY;
    const int spaceWithClose = spaceNoClose - CELL_PAD_Y - BORDER;
    const size_t fitWithClose = spaceWithClose > 0 ? static_cast<size_t>(spaceWithClose / lineHeight) : 0;

    size_t fit;
    bool closing;
    if (remaining <= fitWithClose) {
      fit = remaining;
      closing = true;
    } else {
      fit = spaceNoClose > 0 ? static_cast<size_t>(spaceNoClose / lineHeight) : 0;
      if (fit >= remaining) fit = remaining - 1;  // keep at least one line for the closing segment
      closing = false;
    }
    if (fit == 0) {
      // Degenerate page (shorter than one text line): emit one line per page —
      // it may clip at the page edge, but every line still reaches a page.
      fit = 1;
      closing = remaining == 1;
    }

    for (size_t i = 0; i < rowCellLines.size(); ++i) {
      const auto& lines = rowCellLines[i];
      const size_t end = std::min(lines.size(), emitted + fit);
      for (size_t k = emitted; k < end; ++k) {
        auto line = std::shared_ptr<PageLine>(new (std::nothrow) PageLine(
            lines[k], rowCellX[i], static_cast<int16_t>(contentY + (k - emitted) * lineHeight)));
        if (!line) {
          LOG_ERR("TBL", "OOM: PageLine");
          continue;
        }
        sink.addElement(std::move(line));
      }
    }
    emittedAnything = true;

    const auto contentBottom = static_cast<int16_t>(contentY + fit * lineHeight);
    if (closing) {
      const auto rowBottom = static_cast<int16_t>(contentBottom + CELL_PAD_Y);
      for (const auto bx : rowBoundaries) {
        addRect(sink, bx, segTop, BORDER, static_cast<uint16_t>(rowBottom + BORDER - segTop));
      }
      addRect(sink, tableX, rowBottom, totalWidth, BORDER);
      sink.advanceY(static_cast<int16_t>(rowBottom + BORDER - sink.currentY()));
      break;
    }

    // Row continues on the next page: extend this segment's verticals to the
    // page edge so the grid reads as continuing.
    for (const auto bx : rowBoundaries) {
      addRect(sink, bx, segTop, BORDER, static_cast<uint16_t>(pageH - segTop));
    }
    sink.completePage();
    emitted += fit;
    firstSegment = false;
  }

  // Release this row's line references promptly (the emitted PageLines hold
  // their own); capacity is retained for the next row.
  rowCellLines.clear();
}

void TableLayout::emitRowStacked(Row& row, TablePageSink& sink) {
  for (auto& cell : row) {
    if (cell.text && !cell.text->isEmpty()) {
      emitStackedCell(*cell.text, sink, true);
    }
    cell.text.reset();
  }
  if (stackedRowHadContent) {
    emitStackedRowSeparator(sink);
  }
  stackedRowHadContent = false;
}

void TableLayout::emitStackedCell(ParsedText& text, TablePageSink& sink, const bool includeLastLine) {
  const int16_t pageH = sink.pageHeight();
  const uint16_t contentWidth =
      availableWidth > 2 * CELL_PAD_X ? static_cast<uint16_t>(availableWidth - 2 * CELL_PAD_X) : availableWidth;
  const auto contentX = static_cast<int16_t>(originX + CELL_PAD_X);

  if (topBorderPending) {
    // Open the table block with a gap and a full-width separator.
    if (sink.currentY() > 0) {
      if (sink.currentY() + lineHeight / 2 + BORDER + lineHeight > pageH) {
        sink.completePage();
      } else {
        sink.advanceY(lineHeight / 2);
      }
    }
    addRect(sink, originX, sink.currentY(), availableWidth, BORDER);
    sink.advanceY(static_cast<int16_t>(BORDER + lineHeight / 4));
    topBorderPending = false;
  }

  text.layoutAndExtractLines(
      renderer, fontId, contentWidth,
      [this, &sink, contentX, pageH](const std::shared_ptr<TextBlock>& line) {
        if (sink.currentY() + lineHeight > pageH) {
          sink.completePage();
        }
        auto pageLine = std::shared_ptr<PageLine>(new (std::nothrow) PageLine(line, contentX, sink.currentY()));
        if (!pageLine) {
          LOG_ERR("TBL", "OOM: PageLine");
          return;
        }
        sink.addElement(std::move(pageLine));
        sink.advanceY(lineHeight);
        stackedRowHadContent = true;
        emittedAnything = true;
      },
      includeLastLine);
}

void TableLayout::emitStackedRowSeparator(TablePageSink& sink) {
  // Thin separator under the row (skipped when the page break already separates).
  const auto sepGap = static_cast<int16_t>(lineHeight / 4);
  if (sink.currentY() > 0 && sink.currentY() + 2 * sepGap + BORDER <= sink.pageHeight()) {
    sink.advanceY(sepGap);
    addRect(sink, originX, sink.currentY(), availableWidth, BORDER);
    sink.advanceY(static_cast<int16_t>(BORDER + sepGap));
  }
}
