#include "TableBlock.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

// Layout:
//   1px top border
//   For each row:
//     maxPaddingTop + row.maxLines * lineHeight + maxPaddingBottom  pixels of content
//     1px bottom border (doubles as top border of next row)
int16_t TableBlock::totalHeight() const {
  if (rows.empty()) return 0;
  int16_t h = static_cast<int16_t>(rows.size() + 1);  // (numRows+1) 1px borders
  for (const auto& row : rows) {
    h = static_cast<int16_t>(h + row.maxLines * lineHeight + row.maxPaddingTop + row.maxPaddingBottom);
  }
  return h;
}

void TableBlock::render(GfxRenderer& renderer, int fontId, int x, int y) const {
  if (rows.empty() || numCols == 0) return;
  if (numCols > MAX_COLS) {
    LOG_ERR("TBL", "Skipping table render: too many cols=%u", numCols);
    return;
  }

  // Build cumulative column X positions (numCols+1 values: left edge of each col + right edge of last).
  // numCols <= MAX_COLS (64) validated above, so MAX_COLS+1 entries fit on the stack (130 bytes).
  int16_t colX[MAX_COLS + 1];
  auto cx = static_cast<int16_t>(x);
  for (uint8_t c = 0; c <= numCols; c++) {
    colX[c] = cx;
    if (c < numCols) cx = static_cast<int16_t>(cx + getColWidth(c));
  }
  const int tableWidth = static_cast<int>(colX[numCols]) - x;

  int curY = y;
  if (!rows.empty() && rows.front().drawBorderTop)
    renderer.drawLine(x, curY, x + tableWidth, curY, true);  // top border
  curY += 1;

  // A vertical divider is the right edge of one column and the left edge of the next.
  // Draw it if either the left or right side has borders enabled.
  const bool drawAnyVertical = drawBorderLeft || drawBorderRight || drawInnerColDividers;

  for (size_t ri = 0; ri < rows.size(); ri++) {
    const auto& row = rows[ri];
    const int rowContentH = row.maxLines * lineHeight + row.maxPaddingTop + row.maxPaddingBottom;

    // Vertical column lines — skip interior boundaries of spanning cells.
    // Phantom cells (rowspan==0) are single-column and do not suppress any borders.
    {
      // drawBorderAt[i] == true means draw a vertical line at colX[i].
      // numCols <= MAX_COLS validated above, so MAX_COLS+1 entries are safe on the stack.
      bool drawBorderAt[MAX_COLS + 1] = {};
      // Left edge, interior dividers, and right edge each use their respective flags.
      for (int i = 0; i <= static_cast<int>(numCols); i++) {
        if (i == 0)
          drawBorderAt[i] = drawBorderLeft;
        else if (i == static_cast<int>(numCols))
          drawBorderAt[i] = drawBorderRight;
        else
          drawBorderAt[i] = drawInnerColDividers;  // interior column: cell-border-driven only
      }
      if (drawAnyVertical) {
        uint8_t logCol = 0;
        for (size_t ci = 0; ci < row.cells.size() && logCol < numCols; ci++) {
          const auto& cell = row.cells[ci];
          const uint8_t span = (cell.rowspan == 0) ? 1 : cell.colspan;
          // Interior column positions within this span must not have a divider.
          for (uint8_t k = 1; k < span && (logCol + k) <= numCols; k++) {
            drawBorderAt[logCol + k] = false;
          }
          logCol = static_cast<uint8_t>(logCol + span);
        }
        for (int col = 0; col <= static_cast<int>(numCols); col++) {
          if (drawBorderAt[col]) {
            renderer.drawLine(colX[col], curY, colX[col], curY + rowContentH, true);
          }
        }
      }
    }

    // Cell text — advance logical column by each cell's colspan.
    // Phantom cells (rowspan==0) have no lines, so the inner loop is a no-op for them.
    {
      uint8_t logCol = 0;
      for (size_t ci = 0; ci < row.cells.size() && logCol < numCols; ci++) {
        const auto& cell = row.cells[ci];
        const int cellTextX = static_cast<int>(colX[logCol]) + 1 + cell.paddingLeft;  // +1 for left border
        int lineY = curY + row.maxPaddingTop;
        for (const auto& line : cell.lines) {
          line->render(renderer, fontId, cellTextX, lineY);
          lineY += lineHeight;
        }
        logCol = static_cast<uint8_t>(logCol + ((cell.rowspan == 0) ? 1 : cell.colspan));
      }
    }

    curY += rowContentH;
    // Determine whether to draw a horizontal separator below this row.
    // Inter-row: draw if either the row below has a top border or this row has a bottom border.
    // Last row: draw only if this row has a bottom border.
    const bool drawThisSeparator =
        (ri + 1 < rows.size()) ? (row.drawBorderBottom || rows[ri + 1].drawBorderTop) : row.drawBorderBottom;
    if (drawThisSeparator) {
      // Suppress the bottom border at columns where the next row has phantom cells
      // (those columns are visually spanned by a rowspan cell continuing from this row).
      if (ri + 1 < rows.size()) {
        bool phantomAt[MAX_COLS] = {};
        {
          uint8_t lc = 0;
          for (const auto& nc : rows[ri + 1].cells) {
            if (lc >= numCols) break;
            if (nc.rowspan == 0) phantomAt[lc] = true;
            lc = static_cast<uint8_t>(lc + ((nc.rowspan == 0) ? 1 : nc.colspan));
          }
        }
        int segStart = x;
        for (int col = 0; col < static_cast<int>(numCols); col++) {
          if (phantomAt[col]) {
            if (segStart < colX[col]) renderer.drawLine(segStart, curY, colX[col], curY, true);
            segStart = colX[col + 1];
          }
        }
        if (segStart < colX[numCols]) renderer.drawLine(segStart, curY, colX[numCols], curY, true);
      } else {
        renderer.drawLine(x, curY, x + tableWidth, curY, true);  // last row bottom edge
      }
    }
    curY += 1;
  }
}

bool TableBlock::serialize(HalFile& file) const {
  serialization::writePod(file, numCols);
  serialization::writePod(file, colWidth);
  serialization::writePod(file, lineHeight);
  const uint8_t flags = static_cast<uint8_t>(drawBorderLeft) | static_cast<uint8_t>(drawBorderRight) << 1 |
                        static_cast<uint8_t>(drawInnerColDividers) << 2;
  serialization::writePod(file, flags);

  // Per-column widths
  const auto numColWidths = static_cast<uint8_t>(std::min(colWidths.size(), size_t(255)));
  serialization::writePod(file, numColWidths);
  for (uint8_t c = 0; c < numColWidths; c++) serialization::writePod(file, colWidths[c]);

  const auto numRows = static_cast<uint16_t>(rows.size());
  serialization::writePod(file, numRows);

  for (const auto& row : rows) {
    serialization::writePod(file, row.maxLines);
    serialization::writePod(file, row.maxPaddingTop);
    serialization::writePod(file, row.maxPaddingBottom);
    const uint8_t rowFlags = static_cast<uint8_t>(row.drawBorderTop) | static_cast<uint8_t>(row.drawBorderBottom) << 1;
    serialization::writePod(file, rowFlags);
    const auto numCells = static_cast<uint8_t>(std::min(row.cells.size(), size_t(255)));
    serialization::writePod(file, numCells);
    for (uint8_t c = 0; c < numCells; c++) {
      const auto& cell = row.cells[c];
      serialization::writePod(file, cell.paddingLeft);
      serialization::writePod(file, cell.paddingRight);
      serialization::writePod(file, cell.paddingTop);
      serialization::writePod(file, cell.paddingBottom);
      serialization::writePod(file, cell.colspan);
      serialization::writePod(file, cell.rowspan);
      const auto numLines = static_cast<uint16_t>(std::min(cell.lines.size(), size_t(65535)));
      serialization::writePod(file, numLines);
      for (uint16_t l = 0; l < numLines; l++) {
        if (!cell.lines[l]->serialize(file)) {
          LOG_ERR("TBL", "Failed to serialize cell[%u] line %u", c, l);
          return false;
        }
      }
    }
  }
  return true;
}

std::unique_ptr<TableBlock> TableBlock::deserialize(HalFile& file) {
  auto tb = makeUniqueNoThrow<TableBlock>();
  if (!tb) {
    LOG_ERR("TBL", "OOM: TableBlock");
    return nullptr;
  }
  serialization::readPod(file, tb->numCols);
  serialization::readPod(file, tb->colWidth);
  serialization::readPod(file, tb->lineHeight);
  uint8_t flags = 0;
  serialization::readPod(file, flags);
  tb->drawBorderLeft = (flags & 1) != 0;
  tb->drawBorderRight = (flags & 2) != 0;
  tb->drawInnerColDividers = (flags & 4) != 0;

  // Per-column widths
  uint8_t numColWidths = 0;
  serialization::readPod(file, numColWidths);
  tb->colWidths.resize(numColWidths);
  for (auto& w : tb->colWidths) serialization::readPod(file, w);

  uint16_t numRows = 0;
  serialization::readPod(file, numRows);

  if (numRows > 1000 || tb->numCols > MAX_COLS) {
    LOG_ERR("TBL", "Sanity check failed: rows=%u cols=%u", numRows, tb->numCols);
    return nullptr;
  }

  tb->rows.resize(numRows);
  for (auto& row : tb->rows) {
    serialization::readPod(file, row.maxLines);
    serialization::readPod(file, row.maxPaddingTop);
    serialization::readPod(file, row.maxPaddingBottom);
    uint8_t rowFlags = 0;
    serialization::readPod(file, rowFlags);
    row.drawBorderTop = (rowFlags & 1) != 0;
    row.drawBorderBottom = (rowFlags & 2) != 0;
    uint8_t numCells = 0;
    serialization::readPod(file, numCells);
    row.cells.resize(numCells);
    uint16_t rowSpanTotal = 0;
    for (auto& cell : row.cells) {
      serialization::readPod(file, cell.paddingLeft);
      serialization::readPod(file, cell.paddingRight);
      serialization::readPod(file, cell.paddingTop);
      serialization::readPod(file, cell.paddingBottom);
      serialization::readPod(file, cell.colspan);
      serialization::readPod(file, cell.rowspan);
      if (cell.rowspan == 0)
        cell.colspan = 1;  // phantom cells always span 1 column
      else if (cell.colspan == 0)
        cell.colspan = 1;  // guard against corrupt data
      rowSpanTotal += cell.colspan;
      constexpr uint16_t kMaxCellLines = 4096;
      uint16_t numLines = 0;
      serialization::readPod(file, numLines);
      if (numLines > kMaxCellLines) {
        LOG_ERR("TBL", "Sanity check failed: cell lines=%u", numLines);
        return nullptr;
      }
      cell.lines.reserve(numLines);
      for (uint16_t l = 0; l < numLines; l++) {
        auto line = TextBlock::deserialize(file);
        if (!line) {
          LOG_ERR("TBL", "Failed to deserialize cell line");
          return nullptr;
        }
        cell.lines.push_back(std::move(line));
      }
    }
    if (rowSpanTotal != tb->numCols) {
      LOG_ERR("TBL", "Row span %u != numCols %u", rowSpanTotal, tb->numCols);
      return nullptr;
    }
  }
  return tb;
}
