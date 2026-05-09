#pragma once

#include <HalStorage.h>

#include <memory>
#include <vector>

#include "Block.h"
#include "TextBlock.h"

// One cell in a table row — holds pre-laid-out text lines and its own CSS-resolved padding.
struct TableCell {
  std::vector<std::shared_ptr<TextBlock>> lines;
  int16_t paddingLeft = 2;  // default: TableBlock::CELL_PADDING_X
  int16_t paddingRight = 2;
  int16_t paddingTop = 1;  // default: TableBlock::CELL_PADDING_Y
  int16_t paddingBottom = 1;
  uint8_t colspan = 1;  // number of logical columns this cell spans
  uint8_t rowspan =
      1;  // 0 = phantom (column occupied by rowspan from above), 1 = normal, >1 = spans rows; HTML "0" mapped to 64
};

// One row in a table — holds cells and aggregated height metrics.
struct TableRow {
  std::vector<TableCell> cells;
  uint8_t maxLines = 0;          // max text lines across all cells in this row
  int16_t maxPaddingTop = 1;     // max paddingTop  across all cells
  int16_t maxPaddingBottom = 1;  // max paddingBottom across all cells
};

class GfxRenderer;

// A fully pre-laid-out table. Column widths can be per-column (CSS) or uniform.
// Line heights are pre-computed and stored so rendering does not need to re-query the font.
class TableBlock final : public Block {
 public:
  static constexpr int16_t CELL_PADDING_X = 2;  // horizontal padding default
  static constexpr int16_t CELL_PADDING_Y = 1;  // vertical padding default

  std::vector<TableRow> rows;
  uint8_t numCols = 0;
  int16_t colWidth = 0;            // equal-width fallback (used when colWidths is empty)
  std::vector<int16_t> colWidths;  // per-column pixel widths; if size()==numCols overrides colWidth
  int16_t lineHeight = 0;
  bool drawBorderTop = false;     // draw top edge of table; also gates interior row-separator lines
  bool drawBorderBottom = false;  // draw bottom edge of table; also gates interior row-separator lines
  bool drawBorderLeft = false;    // draw left edge of table; also gates interior column-divider lines
  bool drawBorderRight = false;   // draw right edge of table; also gates interior column-divider lines

  // Returns true if any border side is enabled.
  bool drawAnyBorder() const { return drawBorderTop || drawBorderBottom || drawBorderLeft || drawBorderRight; }

  BlockType getType() override { return TABLE_BLOCK; }
  bool isEmpty() override { return rows.empty(); }

  // Pixel width of column `col` (0-based).
  int16_t getColWidth(uint8_t col) const {
    if (col < static_cast<uint8_t>(colWidths.size())) return colWidths[col];
    return colWidth;
  }

  // Total pixel height occupied by this table (borders + padding + text lines).
  int16_t totalHeight() const;

  void render(GfxRenderer& renderer, int fontId, int x, int y) const;
  bool serialize(FsFile& file) const;
  static std::unique_ptr<TableBlock> deserialize(FsFile& file);
};
