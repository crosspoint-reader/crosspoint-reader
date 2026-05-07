#include "Page.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

void PageLine::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  block->render(renderer, fontId, xPos + xOffset, yPos + yOffset);
}

bool PageLine::serialize(FsFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize TextBlock pointed to by PageLine
  return block->serialize(file);
}

std::unique_ptr<PageLine> PageLine::deserialize(FsFile& file) {
  int16_t xPos;
  int16_t yPos;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);

  auto tb = TextBlock::deserialize(file);
  return std::unique_ptr<PageLine>(new PageLine(std::move(tb), xPos, yPos));
}

void PageImage::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  // Images don't use fontId or text rendering
  imageBlock->render(renderer, xPos + xOffset, yPos + yOffset);
}

bool PageImage::serialize(FsFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize ImageBlock
  return imageBlock->serialize(file);
}

std::unique_ptr<PageImage> PageImage::deserialize(FsFile& file) {
  int16_t xPos;
  int16_t yPos;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);

  auto ib = ImageBlock::deserialize(file);
  return std::unique_ptr<PageImage>(new PageImage(std::move(ib), xPos, yPos));
}

void PageTableFragment::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  const int drawX = xPos + xOffset;
  const int drawY = yPos + yOffset;

  // Outer border
  renderer.drawRect(drawX, drawY, totalWidth, totalHeight, true);

  // Vertical column separators
  int colX = drawX;
  for (uint8_t c = 0; c < columnCount - 1; c++) {
    colX += colWidths[c];
    renderer.drawLine(colX, drawY, colX, drawY + totalHeight - 1, true);
  }

  // Rows: text content + horizontal separators
  int rowY = drawY;
  for (size_t r = 0; r < rows.size(); r++) {
    const TableRow& row = rows[r];
    int cellX = drawX;
    for (uint8_t c = 0; c < columnCount && c < static_cast<uint8_t>(row.cells.size()); c++) {
      const TableCell& cell = row.cells[c];
      int lineY = rowY + TABLE_CELL_PADDING;
      for (const auto& line : cell.lines) {
        line->render(renderer, fontId, cellX + TABLE_CELL_PADDING, lineY);
        lineY += renderer.getLineHeight(fontId);
      }
      cellX += colWidths[c];
    }
    rowY += row.height;
    // Draw horizontal separator (skip after last row — outer border covers it)
    if (r + 1 < rows.size()) {
      const int sepLineWidth = row.isHeaderRow ? 2 : 1;
      renderer.drawLine(drawX, rowY, drawX + totalWidth - 1, rowY, sepLineWidth, true);
    }
  }
}

bool PageTableFragment::serialize(FsFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);
  serialization::writePod(file, columnCount);
  serialization::writePod(file, totalWidth);
  serialization::writePod(file, totalHeight);
  for (uint8_t c = 0; c < MAX_TABLE_COLS; c++) {
    serialization::writePod(file, colWidths[c]);
  }
  const uint16_t rowCount = static_cast<uint16_t>(rows.size());
  serialization::writePod(file, rowCount);
  for (const auto& row : rows) {
    serialization::writePod(file, row.height);
    serialization::writePod(file, row.isHeaderRow);
    const uint8_t cellCount = static_cast<uint8_t>(row.cells.size());
    serialization::writePod(file, cellCount);
    for (const auto& cell : row.cells) {
      serialization::writePod(file, cell.isHeader);
      const uint8_t lineCount = static_cast<uint8_t>(cell.lines.size());
      serialization::writePod(file, lineCount);
      for (const auto& line : cell.lines) {
        if (!line->serialize(file)) return false;
      }
    }
  }
  return true;
}

std::unique_ptr<PageTableFragment> PageTableFragment::deserialize(FsFile& file) {
  int16_t xPos, yPos;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);

  uint8_t columnCount;
  uint16_t totalWidth, totalHeight;
  serialization::readPod(file, columnCount);
  serialization::readPod(file, totalWidth);
  serialization::readPod(file, totalHeight);

  if (columnCount == 0 || columnCount > MAX_TABLE_COLS) {
    LOG_ERR("PGE", "TableFragment: invalid columnCount %u", columnCount);
    return nullptr;
  }

  std::array<uint16_t, MAX_TABLE_COLS> colWidths = {};
  for (uint8_t c = 0; c < MAX_TABLE_COLS; c++) {
    serialization::readPod(file, colWidths[c]);
  }

  uint16_t rowCount;
  serialization::readPod(file, rowCount);
  if (rowCount > MAX_TABLE_ROWS) {
    LOG_ERR("PGE", "TableFragment: invalid rowCount %u", rowCount);
    return nullptr;
  }

  std::vector<TableRow> rows;
  rows.reserve(rowCount);
  for (uint16_t r = 0; r < rowCount; r++) {
    TableRow row;
    serialization::readPod(file, row.height);
    serialization::readPod(file, row.isHeaderRow);
    uint8_t cellCount;
    serialization::readPod(file, cellCount);
    if (cellCount > MAX_TABLE_COLS) {
      LOG_ERR("PGE", "TableFragment: invalid cellCount %u in row %u", cellCount, r);
      return nullptr;
    }
    row.cells.reserve(cellCount);
    for (uint8_t c = 0; c < cellCount; c++) {
      TableCell cell;
      serialization::readPod(file, cell.isHeader);
      uint8_t lineCount;
      serialization::readPod(file, lineCount);
      if (lineCount > MAX_CELL_LINES) {
        LOG_ERR("PGE", "TableFragment: invalid lineCount %u at row %u cell %u", lineCount, r, c);
        return nullptr;
      }
      cell.lines.reserve(lineCount);
      for (uint8_t l = 0; l < lineCount; l++) {
        auto tb = TextBlock::deserialize(file);
        if (!tb) {
          LOG_ERR("PGE", "TableFragment: TextBlock deserialize failed at row %u cell %u line %u", r, c, l);
          return nullptr;
        }
        cell.lines.push_back(std::move(tb));
      }
      row.cells.push_back(std::move(cell));
    }
    rows.push_back(std::move(row));
  }

  return std::unique_ptr<PageTableFragment>(
      new PageTableFragment(columnCount, totalWidth, totalHeight, colWidths, std::move(rows), xPos, yPos));
}

void Page::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) const {
  for (auto& element : elements) {
    element->render(renderer, fontId, xOffset, yOffset);
  }
}

bool Page::serialize(FsFile& file) const {
  const uint16_t count = elements.size();
  serialization::writePod(file, count);

  for (const auto& el : elements) {
    // Use getTag() method to determine type
    serialization::writePod(file, static_cast<uint8_t>(el->getTag()));

    if (!el->serialize(file)) {
      return false;
    }
  }

  // Serialize footnotes (clamp to MAX_FOOTNOTES_PER_PAGE to match addFootnote/deserialize limits)
  const uint16_t fnCount = std::min<uint16_t>(footnotes.size(), MAX_FOOTNOTES_PER_PAGE);
  serialization::writePod(file, fnCount);
  for (uint16_t i = 0; i < fnCount; i++) {
    const auto& fn = footnotes[i];
    if (file.write(fn.number, sizeof(fn.number)) != sizeof(fn.number) ||
        file.write(fn.href, sizeof(fn.href)) != sizeof(fn.href)) {
      LOG_ERR("PGE", "Failed to write footnote");
      return false;
    }
  }

  return true;
}

std::unique_ptr<Page> Page::deserialize(FsFile& file) {
  auto page = std::unique_ptr<Page>(new Page());

  uint16_t count;
  serialization::readPod(file, count);

  page->elements.reserve(count);
  for (uint16_t i = 0; i < count; i++) {
    uint8_t tag;
    serialization::readPod(file, tag);

    if (tag == TAG_PageLine) {
      auto pl = PageLine::deserialize(file);
      page->elements.push_back(std::move(pl));
    } else if (tag == TAG_PageImage) {
      auto pi = PageImage::deserialize(file);
      page->elements.push_back(std::move(pi));
    } else if (tag == TAG_PageTable) {
      auto pt = PageTableFragment::deserialize(file);
      if (!pt) return nullptr;
      page->elements.push_back(std::move(pt));
    } else {
      LOG_ERR("PGE", "Deserialization failed: Unknown tag %u", tag);
      return nullptr;
    }
  }

  // Deserialize footnotes
  uint16_t fnCount;
  serialization::readPod(file, fnCount);
  if (fnCount > MAX_FOOTNOTES_PER_PAGE) {
    LOG_ERR("PGE", "Invalid footnote count %u", fnCount);
    return nullptr;
  }
  page->footnotes.resize(fnCount);
  for (uint16_t i = 0; i < fnCount; i++) {
    auto& entry = page->footnotes[i];
    if (file.read(entry.number, sizeof(entry.number)) != sizeof(entry.number) ||
        file.read(entry.href, sizeof(entry.href)) != sizeof(entry.href)) {
      LOG_ERR("PGE", "Failed to read footnote %u", i);
      return nullptr;
    }
    entry.number[sizeof(entry.number) - 1] = '\0';
    entry.href[sizeof(entry.href) - 1] = '\0';
  }

  return page;
}
