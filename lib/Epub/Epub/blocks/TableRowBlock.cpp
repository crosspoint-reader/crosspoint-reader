#include "TableRowBlock.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

TableRowBlock::TableRowBlock(std::vector<std::vector<std::string>> cellLines, std::vector<bool> cellIsHeader,
                             std::shared_ptr<TableColumnLayout> layout, const int16_t rowHeight, const bool isFirstRow,
                             const bool isLastRow)
    : cellLines(std::move(cellLines)),
      cellIsHeader(std::move(cellIsHeader)),
      layout(std::move(layout)),
      rowHeight(rowHeight),
      isFirstRow(isFirstRow),
      isLastRow(isLastRow) {}

void TableRowBlock::render(GfxRenderer& renderer, const int x, const int y, const int viewportWidth) const {
  if (!layout || layout->colWidths.empty()) return;

  const int fontId = layout->fontId;
  const int16_t lineH = layout->lineHeight;
  const int16_t pad = layout->cellPadding;
  const int numCols = static_cast<int>(layout->colWidths.size());

  // Left-align table
  int totalTableWidth = 0;
  for (auto w : layout->colWidths) totalTableWidth += w;
  const int tableX = x;

  // Draw top border for first row
  if (isFirstRow) {
    renderer.drawLine(tableX, y, tableX + totalTableWidth, y, true);
  }

  // Draw each cell
  int cellX = tableX;
  for (int col = 0; col < numCols; col++) {
    const int colW = layout->colWidths[col];

    // Left vertical border
    renderer.drawLine(cellX, y, cellX, y + rowHeight, true);

    // Cell text lines
    if (col < static_cast<int>(cellLines.size())) {
      const auto style = (col < static_cast<int>(cellIsHeader.size()) && cellIsHeader[col]) ? EpdFontFamily::BOLD
                                                                                            : EpdFontFamily::REGULAR;
      int lineY = y + pad;
      for (const auto& line : cellLines[col]) {
        if (!line.empty()) {
          renderer.drawText(fontId, cellX + pad, lineY, line.c_str(), true, style);
        }
        lineY += lineH;
      }
    }

    cellX += colW;
  }

  // Right border of last column
  renderer.drawLine(cellX, y, cellX, y + rowHeight, true);

  // Bottom border
  renderer.drawLine(tableX, y + rowHeight, tableX + totalTableWidth, y + rowHeight, true);
}

bool TableRowBlock::serialize(FsFile& file) const {
  const auto cellCount = static_cast<uint16_t>(cellLines.size());
  serialization::writePod(file, cellCount);

  // Per cell: line count + lines
  for (const auto& lines : cellLines) {
    const auto lineCount = static_cast<uint16_t>(lines.size());
    serialization::writePod(file, lineCount);
    for (const auto& line : lines) serialization::writeString(file, line);
  }

  // Header flags
  for (uint16_t i = 0; i < cellCount; i++) {
    bool isHeader = (i < cellIsHeader.size()) ? cellIsHeader[i] : false;
    serialization::writePod(file, isHeader);
  }

  // Layout
  const auto colCount = static_cast<uint16_t>(layout ? layout->colWidths.size() : 0);
  serialization::writePod(file, colCount);
  if (layout) {
    for (auto w : layout->colWidths) serialization::writePod(file, w);
    serialization::writePod(file, layout->fontId);
    serialization::writePod(file, layout->lineHeight);
    serialization::writePod(file, layout->cellPadding);
  }

  serialization::writePod(file, rowHeight);
  serialization::writePod(file, isFirstRow);
  serialization::writePod(file, isLastRow);

  return true;
}

std::unique_ptr<TableRowBlock> TableRowBlock::deserialize(FsFile& file) {
  uint16_t cellCount;
  serialization::readPod(file, cellCount);
  if (cellCount > 200) {
    LOG_ERR("TRB", "Cell count %u exceeds max", cellCount);
    return nullptr;
  }

  std::vector<std::vector<std::string>> allLines(cellCount);
  for (uint16_t i = 0; i < cellCount; i++) {
    uint16_t lineCount;
    serialization::readPod(file, lineCount);
    if (lineCount > 50) {
      LOG_ERR("TRB", "Line count %u exceeds max", lineCount);
      return nullptr;
    }
    allLines[i].resize(lineCount);
    for (auto& line : allLines[i]) serialization::readString(file, line);
  }

  std::vector<bool> headers(cellCount);
  for (uint16_t i = 0; i < cellCount; i++) {
    bool h;
    serialization::readPod(file, h);
    headers[i] = h;
  }

  uint16_t colCount;
  serialization::readPod(file, colCount);
  auto layout = std::make_shared<TableColumnLayout>();
  layout->colWidths.resize(colCount);
  for (auto& w : layout->colWidths) serialization::readPod(file, w);
  serialization::readPod(file, layout->fontId);
  serialization::readPod(file, layout->lineHeight);
  serialization::readPod(file, layout->cellPadding);

  int16_t rh;
  serialization::readPod(file, rh);
  bool firstRow, lastRow;
  serialization::readPod(file, firstRow);
  serialization::readPod(file, lastRow);

  return std::unique_ptr<TableRowBlock>(
      new TableRowBlock(std::move(allLines), std::move(headers), std::move(layout), rh, firstRow, lastRow));
}
