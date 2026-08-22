#include "TerminalScreen.h"

#include <algorithm>
#include <cstring>
#include <iterator>

TerminalScreen::TerminalScreen() { reset(); }

void TerminalScreen::clearDirtySpans() {
  std::fill(std::begin(dirtyFirstColumn), std::end(dirtyFirstColumn), COLS);
  std::fill(std::begin(dirtyLastColumn), std::end(dirtyLastColumn), 0);
}

void TerminalScreen::markCellDirty(const uint8_t row, const uint8_t column) { markRangeDirty(row, column, column); }

void TerminalScreen::markRangeDirty(const uint8_t row, const uint8_t firstColumn, const uint8_t lastColumn) {
  dirtyRows |= uint32_t{1} << row;
  dirtyFirstColumn[row] = std::min(dirtyFirstColumn[row], firstColumn);
  dirtyLastColumn[row] = std::max(dirtyLastColumn[row], lastColumn);
}

void TerminalScreen::reset() {
  for (auto& row : cells) {
    std::fill(std::begin(row), std::end(row), BLANK_CELL);
  }
  cursorRow = 0;
  cursorColumn = 0;
  currentAttributes = ATTR_NONE;
  cursorVisible = true;
  wrapPending = false;
  clearDirtySpans();
  markAllDirty();
}

void TerminalScreen::markAllDirty() {
  dirtyRows = (uint32_t{1} << ROWS) - 1;
  for (uint8_t row = 0; row < ROWS; ++row) {
    dirtyFirstColumn[row] = 0;
    dirtyLastColumn[row] = COLS - 1;
  }
}

TerminalScreen::DirtyRegion TerminalScreen::takeDirtyRegion() {
  DirtyRegion result;
  result.rows = dirtyRows;
  std::copy(std::begin(dirtyFirstColumn), std::end(dirtyFirstColumn), std::begin(result.firstColumn));
  std::copy(std::begin(dirtyLastColumn), std::end(dirtyLastColumn), std::begin(result.lastColumn));
  dirtyRows = 0;
  clearDirtySpans();
  return result;
}

TerminalScreen::DirtyRegion TerminalScreen::takeDirtyRegionComparedTo(const TerminalScreen& previous) {
  DirtyRegion result = takeDirtyRegion();
  for (uint8_t row = 0; row < ROWS; ++row) {
    const uint32_t bit = uint32_t{1} << row;
    if ((result.rows & bit) == 0) continue;

    int first = result.firstColumn[row];
    int last = result.lastColumn[row];
    while (first <= last && visuallyMatches(previous, row, static_cast<uint8_t>(first))) ++first;
    while (last >= first && visuallyMatches(previous, row, static_cast<uint8_t>(last))) --last;
    if (first > last) {
      result.rows &= ~bit;
    } else {
      result.firstColumn[row] = static_cast<uint8_t>(first);
      result.lastColumn[row] = static_cast<uint8_t>(last);
    }
  }
  return result;
}

bool TerminalScreen::visuallyMatches(const TerminalScreen& previous, const uint8_t row, const uint8_t column) const {
  const Cell& currentCell = cells[row][column];
  const Cell& previousCell = previous.cells[row][column];
  if (currentCell.codepoint != previousCell.codepoint || currentCell.attributes != previousCell.attributes) {
    return false;
  }
  const bool currentCursor = cursorVisible && cursorRow == row && cursorColumn == column;
  const bool previousCursor = previous.cursorVisible && previous.cursorRow == row && previous.cursorColumn == column;
  return currentCursor == previousCursor;
}

void TerminalScreen::putCodepoint(uint32_t codepoint, const uint8_t scrollTop, const uint8_t scrollBottom) {
  if (codepoint < 0x20 || codepoint > 0xffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
    codepoint = REPLACEMENT_CODEPOINT;
  }

  // VT terminals defer wrapping until the next printable character. An exact
  // width line followed by CRLF therefore advances only once.
  if (wrapPending) {
    cursorColumn = 0;
    lineFeed(scrollTop, scrollBottom);
  }

  const uint8_t oldColumn = cursorColumn;
  markCellDirty(cursorRow, oldColumn);
  cells[cursorRow][oldColumn] = Cell{static_cast<uint16_t>(codepoint), currentAttributes};
  if (oldColumn + 1 >= COLS) {
    wrapPending = true;
  } else {
    ++cursorColumn;
    markCellDirty(cursorRow, cursorColumn);
  }
}

void TerminalScreen::cancelPendingWrap() { wrapPending = false; }

void TerminalScreen::scrollUp(const uint8_t scrollTop, const uint8_t scrollBottom, uint8_t count) {
  if (scrollTop >= ROWS || scrollBottom >= ROWS || scrollTop > scrollBottom || count == 0) return;

  const uint8_t height = scrollBottom - scrollTop + 1;
  count = std::min(count, height);
  if (count < height) {
    std::memmove(cells[scrollTop], cells[scrollTop + count], sizeof(Cell) * COLS * (height - count));
  }
  for (uint8_t row = scrollBottom - count + 1; row <= scrollBottom; ++row) {
    std::fill(std::begin(cells[row]), std::end(cells[row]), BLANK_CELL);
  }
  for (uint8_t row = scrollTop; row <= scrollBottom; ++row) markRangeDirty(row, 0, COLS - 1);
}

void TerminalScreen::scrollDown(const uint8_t scrollTop, const uint8_t scrollBottom, uint8_t count) {
  if (scrollTop >= ROWS || scrollBottom >= ROWS || scrollTop > scrollBottom || count == 0) return;

  const uint8_t height = scrollBottom - scrollTop + 1;
  count = std::min(count, height);
  if (count < height) {
    std::memmove(cells[scrollTop + count], cells[scrollTop], sizeof(Cell) * COLS * (height - count));
  }
  for (uint8_t row = scrollTop; row < scrollTop + count; ++row) {
    std::fill(std::begin(cells[row]), std::end(cells[row]), BLANK_CELL);
  }
  for (uint8_t row = scrollTop; row <= scrollBottom; ++row) markRangeDirty(row, 0, COLS - 1);
}

void TerminalScreen::lineFeed(const uint8_t scrollTop, const uint8_t scrollBottom) {
  cancelPendingWrap();
  markCellDirty(cursorRow, cursorColumn);
  if (cursorRow == scrollBottom && cursorRow >= scrollTop) {
    scrollUp(scrollTop, scrollBottom);
  } else if (cursorRow + 1 < ROWS) {
    ++cursorRow;
    markCellDirty(cursorRow, cursorColumn);
  }
}

void TerminalScreen::reverseIndex(const uint8_t scrollTop, const uint8_t scrollBottom) {
  cancelPendingWrap();
  markCellDirty(cursorRow, cursorColumn);
  if (cursorRow == scrollTop && cursorRow <= scrollBottom) {
    scrollDown(scrollTop, scrollBottom);
  } else if (cursorRow > 0) {
    --cursorRow;
    markCellDirty(cursorRow, cursorColumn);
  }
}

void TerminalScreen::carriageReturn() {
  cancelPendingWrap();
  markCellDirty(cursorRow, cursorColumn);
  cursorColumn = 0;
  markCellDirty(cursorRow, cursorColumn);
}

void TerminalScreen::backspace() {
  cancelPendingWrap();
  if (cursorColumn > 0) {
    markCellDirty(cursorRow, cursorColumn);
    --cursorColumn;
    markCellDirty(cursorRow, cursorColumn);
  }
}

void TerminalScreen::tab() {
  cancelPendingWrap();
  const uint8_t spaces = static_cast<uint8_t>(TAB_WIDTH - (cursorColumn % TAB_WIDTH));
  for (uint8_t i = 0; i < spaces; ++i) putCodepoint(' ');
}

void TerminalScreen::moveCursor(const int rowDelta, const int columnDelta) {
  cancelPendingWrap();
  markCellDirty(cursorRow, cursorColumn);
  cursorRow = static_cast<uint8_t>(std::clamp(static_cast<int>(cursorRow) + rowDelta, 0, ROWS - 1));
  cursorColumn = static_cast<uint8_t>(std::clamp(static_cast<int>(cursorColumn) + columnDelta, 0, COLS - 1));
  markCellDirty(cursorRow, cursorColumn);
}

void TerminalScreen::setCursor(const uint16_t oneBasedRow, const uint16_t oneBasedColumn) {
  cancelPendingWrap();
  markCellDirty(cursorRow, cursorColumn);
  const uint16_t row = oneBasedRow == 0 ? 0 : oneBasedRow - 1;
  const uint16_t column = oneBasedColumn == 0 ? 0 : oneBasedColumn - 1;
  cursorRow = static_cast<uint8_t>(std::min<uint16_t>(row, ROWS - 1));
  cursorColumn = static_cast<uint8_t>(std::min<uint16_t>(column, COLS - 1));
  markCellDirty(cursorRow, cursorColumn);
}

void TerminalScreen::setCursorVisible(const bool visible) {
  if (cursorVisible != visible) {
    cursorVisible = visible;
    markCellDirty(cursorRow, cursorColumn);
  }
}

void TerminalScreen::clearRange(const uint8_t row, const uint8_t firstColumn, const uint8_t lastColumn) {
  std::fill(cells[row] + firstColumn, cells[row] + lastColumn + 1, BLANK_CELL);
  markRangeDirty(row, firstColumn, lastColumn);
}

void TerminalScreen::clearDisplay(const uint16_t mode) {
  cancelPendingWrap();
  if (mode == 2 || mode == 3) {
    for (uint8_t row = 0; row < ROWS; ++row) clearRange(row, 0, COLS - 1);
    return;
  }

  if (mode == 1) {
    for (uint8_t row = 0; row < cursorRow; ++row) clearRange(row, 0, COLS - 1);
    clearRange(cursorRow, 0, cursorColumn);
    return;
  }

  clearRange(cursorRow, cursorColumn, COLS - 1);
  for (uint8_t row = cursorRow + 1; row < ROWS; ++row) clearRange(row, 0, COLS - 1);
}

void TerminalScreen::clearLine(const uint16_t mode) {
  cancelPendingWrap();
  if (mode == 1) {
    clearRange(cursorRow, 0, cursorColumn);
  } else if (mode == 2) {
    clearRange(cursorRow, 0, COLS - 1);
  } else {
    clearRange(cursorRow, cursorColumn, COLS - 1);
  }
}
