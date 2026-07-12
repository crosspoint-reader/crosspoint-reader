#include "SudokuActivity.h"

#include <I18n.h>
#include <esp_random.h>

#include <cstring>
#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

// 25% gray (light) — every other pixel in checkerboard on even rows only
static void fillDithered25(GfxRenderer& r, int x, int y, int w, int h) {
  for (int dy = 0; dy < h; dy += 2)
    for (int dx = ((dy/2) % 2); dx < w; dx += 2)
      r.drawPixel(x + dx, y + dy, true);
}

bool SudokuActivity::isValid(const uint8_t b[9][9], int row, int col, uint8_t num) const {
  for (int i = 0; i < 9; i++) {
    if (b[row][i] == num) return false;
    if (b[i][col] == num) return false;
  }
  int boxR = (row / 3) * 3, boxC = (col / 3) * 3;
  for (int r = boxR; r < boxR + 3; r++) {
    for (int c = boxC; c < boxC + 3; c++) {
      if (b[r][c] == num) return false;
    }
  }
  return true;
}

bool SudokuActivity::solve(uint8_t b[9][9]) {
  for (int r = 0; r < 9; r++) {
    for (int c = 0; c < 9; c++) {
      if (b[r][c] == 0) {
        for (uint8_t n = 1; n <= 9; n++) {
          if (isValid(b, r, c, n)) {
            b[r][c] = n;
            if (solve(b)) return true;
            b[r][c] = 0;
          }
        }
        return false;
      }
    }
  }
  return true;
}

void SudokuActivity::generatePuzzle() {
  // Start with empty board, fill diagonal boxes randomly, then solve
  memset(board, 0, sizeof(board));
  memset(fixed, 0, sizeof(fixed));

  // Fill diagonal 3x3 boxes (they don't constrain each other)
  for (int box = 0; box < 3; box++) {
    uint8_t nums[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    // Fisher-Yates shuffle
    for (int i = 8; i > 0; i--) {
      int j = esp_random() % (i + 1);
      uint8_t tmp = nums[i];
      nums[i] = nums[j];
      nums[j] = tmp;
    }
    int idx = 0;
    for (int r = box * 3; r < box * 3 + 3; r++) {
      for (int c = box * 3; c < box * 3 + 3; c++) {
        board[r][c] = nums[idx++];
      }
    }
  }

  // Solve the rest
  solve(board);

  // Remove cells to create puzzle (keep ~30 clues for medium difficulty)
  int toRemove = 51;  // 81 - 30 = 51 cells removed
  while (toRemove > 0) {
    int r = esp_random() % 9;
    int c = esp_random() % 9;
    if (board[r][c] != 0) {
      board[r][c] = 0;
      toRemove--;
    }
  }

  // Mark remaining as fixed
  for (int r = 0; r < 9; r++) {
    for (int c = 0; c < 9; c++) {
      fixed[r][c] = (board[r][c] != 0);
    }
  }
}

void SudokuActivity::onEnter() {
  Activity::onEnter();
  state = PLAYING;
  cursorX = 0;
  cursorY = 0;
  generatePuzzle();
  requestUpdate();
}

void SudokuActivity::loop() {
  if (state == SOLVED || state == NO_SOLUTION) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state = PLAYING;
      generatePuzzle();
      requestUpdate();
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  bool moved = false;

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (cursorY > 0) cursorY--;
    moved = true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (cursorY < 8) cursorY++;
    moved = true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (cursorX > 0) cursorX--;
    moved = true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (cursorX < 8) cursorX++;
    moved = true;
  }

  // Confirm: cycle number (0->1->2->...->9->0) on non-fixed cells
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!fixed[cursorY][cursorX]) {
      if (mappedInput.getHeldTime() >= 500) {
        // Long press: auto-solve
        uint8_t copy[9][9];
        memcpy(copy, board, sizeof(board));
        if (solve(copy)) {
          memcpy(board, copy, sizeof(board));
          state = SOLVED;
        } else {
          state = NO_SOLUTION;
        }
      } else {
        board[cursorY][cursorX] = (board[cursorY][cursorX] % 9) + 1;
      }
      moved = true;
    }
  }

  // PageForward: clear cell
  if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
    if (!fixed[cursorY][cursorX]) {
      board[cursorY][cursorX] = 0;
      moved = true;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (moved) requestUpdate();
}

void SudokuActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SUDOKU));

  const int headerBottom = metrics.topPadding + metrics.headerHeight + 4;
  const int hintsTop = pageHeight - metrics.buttonHintsHeight;
  const int availHeight = hintsTop - headerBottom - 4;
  const int availWidth = pageWidth - 2 * metrics.contentSidePadding;

  int cellSize = std::min(availWidth / 9, availHeight / 9);
  if (cellSize > 50) cellSize = 50;

  const int gridSize = cellSize * 9;
  const int gridX = (pageWidth - gridSize) / 2;
  const int gridY = headerBottom + (availHeight - gridSize) / 2;

  const int fontH = renderer.getTextHeight(SMALL_FONT_ID);

  // Draw cells
  for (int r = 0; r < 9; r++) {
    for (int c = 0; c < 9; c++) {
      int px = gridX + c * cellSize;
      int py = gridY + r * cellSize;
      bool isCursor = (c == cursorX && r == cursorY && state == PLAYING);

      if (isCursor) {
        // 3px thick cursor border
        renderer.drawRect(px, py, cellSize, cellSize, true);
        renderer.drawRect(px + 1, py + 1, cellSize - 2, cellSize - 2, true);
        renderer.drawRect(px + 2, py + 2, cellSize - 4, cellSize - 4, true);
      }

      // Fixed cells get light dithered background
      if (fixed[r][c] && !isCursor) {
        fillDithered25(renderer, px, py, cellSize, cellSize);
      }

      // Highlight cells with same number as cursor
      if (state == PLAYING && !isCursor && board[r][c] > 0 &&
          board[cursorY][cursorX] > 0 && board[r][c] == board[cursorY][cursorX]) {
        fillDithered25(renderer, px, py, cellSize, cellSize);
      }

      if (board[r][c] > 0) {
        char num[2] = {static_cast<char>('0' + board[r][c]), 0};
        EpdFontFamily::Style style = fixed[r][c] ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
        int tw = renderer.getTextWidth(SMALL_FONT_ID, num, style);
        renderer.drawText(SMALL_FONT_ID, px + (cellSize - tw) / 2, py + (cellSize - fontH) / 2, num, true,
                          style);
      }
    }
  }

  // Draw grid lines
  for (int i = 0; i <= 9; i++) {
    int lx = gridX + i * cellSize;
    int ly = gridY + i * cellSize;
    bool thick = (i % 3 == 0);

    // Vertical line
    if (thick) {
      renderer.fillRect(lx - 1, gridY, 3, gridSize, true);
    } else {
      renderer.drawLine(lx, gridY, lx, gridY + gridSize);
    }

    // Horizontal line
    if (thick) {
      renderer.fillRect(gridX, ly - 1, gridSize, 3, true);
    } else {
      renderer.drawLine(gridX, ly, gridX + gridSize, ly);
    }
  }

  // Overlay for solved/no solution
  if (state == SOLVED) {
    GUI.drawPopup(renderer, tr(STR_SOLVED));
  } else if (state == NO_SOLUTION) {
    GUI.drawPopup(renderer, tr(STR_NO_SOLUTION));
  }

  if (state == PLAYING) {
    const auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, "Hold: Solve", labels.btn3, labels.btn4);
  } else {
    const auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_NEW_GAME), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
