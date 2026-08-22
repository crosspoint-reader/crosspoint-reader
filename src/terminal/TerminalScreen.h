#pragma once

#include <cstddef>
#include <cstdint>

class TerminalScreen {
 public:
  static constexpr uint8_t COLS = 80;
  static constexpr uint8_t ROWS = 24;
  static constexpr uint8_t TAB_WIDTH = 8;
  static constexpr uint16_t REPLACEMENT_CODEPOINT = 0xfffd;

  enum Attribute : uint8_t {
    ATTR_NONE = 0,
    ATTR_BOLD = 1 << 0,
    ATTR_INVERSE = 1 << 1,
    ATTR_UNDERLINE = 1 << 2,
  };

  struct Cell {
    uint16_t codepoint;
    uint8_t attributes;
  };

  struct DirtyRegion {
    uint32_t rows = 0;
    uint8_t firstColumn[ROWS]{};
    uint8_t lastColumn[ROWS]{};

    bool empty() const { return rows == 0; }
  };

  TerminalScreen();

  void reset();
  void putCodepoint(uint32_t codepoint, uint8_t scrollTop = 0, uint8_t scrollBottom = ROWS - 1);
  void lineFeed(uint8_t scrollTop = 0, uint8_t scrollBottom = ROWS - 1);
  void reverseIndex(uint8_t scrollTop = 0, uint8_t scrollBottom = ROWS - 1);
  void scrollUp(uint8_t scrollTop, uint8_t scrollBottom, uint8_t count = 1);
  void scrollDown(uint8_t scrollTop, uint8_t scrollBottom, uint8_t count = 1);
  void carriageReturn();
  void backspace();
  void tab();

  void moveCursor(int rowDelta, int columnDelta);
  void setCursor(uint16_t oneBasedRow, uint16_t oneBasedColumn);
  void setCursorVisible(bool visible);
  void clearDisplay(uint16_t mode);
  void clearLine(uint16_t mode);

  void setAttributes(uint8_t attributes) { currentAttributes = attributes; }
  uint8_t getAttributes() const { return currentAttributes; }
  bool isCursorVisible() const { return cursorVisible; }
  uint8_t getCursorRow() const { return cursorRow; }
  uint8_t getCursorColumn() const { return cursorColumn; }
  const Cell& getCell(uint8_t row, uint8_t column) const { return cells[row][column]; }

  DirtyRegion takeDirtyRegion();
  DirtyRegion takeDirtyRegionComparedTo(const TerminalScreen& previous);
  bool hasDirtyRows() const { return dirtyRows != 0; }
  void markAllDirty();

 private:
  Cell cells[ROWS][COLS];
  uint8_t cursorRow = 0;
  uint8_t cursorColumn = 0;
  uint8_t currentAttributes = ATTR_NONE;
  bool cursorVisible = true;
  bool wrapPending = false;
  uint32_t dirtyRows = 0;
  uint8_t dirtyFirstColumn[ROWS]{};
  uint8_t dirtyLastColumn[ROWS]{};

  static constexpr Cell BLANK_CELL{' ', ATTR_NONE};

  void clearDirtySpans();
  void markCellDirty(uint8_t row, uint8_t column);
  void markRangeDirty(uint8_t row, uint8_t firstColumn, uint8_t lastColumn);
  bool visuallyMatches(const TerminalScreen& previous, uint8_t row, uint8_t column) const;
  void cancelPendingWrap();
  void clearRange(uint8_t row, uint8_t firstColumn, uint8_t lastColumn);
};

static_assert(sizeof(TerminalScreen::Cell) == 4);
