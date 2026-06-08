#include "SudokuActivity.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

static constexpr const char* SAVE_PATH = "/sudoku/save.json";
static constexpr unsigned long LONG_PRESS_MS = 600;

// Clues remaining per difficulty
static constexpr int CLUE_COUNT[4] = {36, 30, 26, 22};

// Base grid (valid complete Sudoku)
static constexpr uint8_t BASE_GRID[9][9] = {
    {1, 2, 3, 4, 5, 6, 7, 8, 9}, {4, 5, 6, 7, 8, 9, 1, 2, 3}, {7, 8, 9, 1, 2, 3, 4, 5, 6},
    {2, 3, 4, 5, 6, 7, 8, 9, 1}, {5, 6, 7, 8, 9, 1, 2, 3, 4}, {8, 9, 1, 2, 3, 4, 5, 6, 7},
    {3, 4, 5, 6, 7, 8, 9, 1, 2}, {6, 7, 8, 9, 1, 2, 3, 4, 5}, {9, 1, 2, 3, 4, 5, 6, 7, 8},
};

SudokuActivity::SudokuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Sudoku", renderer, mappedInput) {}

const char* SudokuActivity::difficultyName(Difficulty d) {
  switch (d) {
    case Difficulty::EASY:
      return tr(STR_SUDOKU_EASY);
    case Difficulty::MEDIUM:
      return tr(STR_SUDOKU_MEDIUM);
    case Difficulty::HARD:
      return tr(STR_SUDOKU_HARD);
    case Difficulty::EXPERT:
      return tr(STR_SUDOKU_EXPERT);
  }
  return "";
}

uint32_t SudokuActivity::elapsedSeconds() const {
  if (state == State::PLAY || state == State::SELECT_NUMBER) {
    return pausedElapsed + (millis() - startTime) / 1000;
  }
  return pausedElapsed;
}

// Fisher-Yates shuffle for a 3-element array
static void shuffle3(uint8_t arr[3]) {
  for (int i = 2; i > 0; --i) {
    int j = rand() % (i + 1);
    uint8_t tmp = arr[i];
    arr[i] = arr[j];
    arr[j] = tmp;
  }
}

void SudokuActivity::applyPermutation(uint8_t grid[GRID][GRID]) {
  // Shuffle rows within each band
  for (int band = 0; band < 3; ++band) {
    uint8_t rows[3] = {0, 1, 2};
    shuffle3(rows);
    uint8_t tmp[3][GRID];
    for (int r = 0; r < 3; ++r) {
      memcpy(tmp[r], grid[band * 3 + rows[r]], GRID);
    }
    for (int r = 0; r < 3; ++r) {
      memcpy(grid[band * 3 + r], tmp[r], GRID);
    }
  }

  // Shuffle bands
  uint8_t bands[3] = {0, 1, 2};
  shuffle3(bands);
  uint8_t tmpGrid[GRID][GRID];
  for (int b = 0; b < 3; ++b) {
    for (int r = 0; r < 3; ++r) {
      memcpy(tmpGrid[b * 3 + r], grid[bands[b] * 3 + r], GRID);
    }
  }
  memcpy(grid, tmpGrid, sizeof(tmpGrid));

  // Shuffle columns within each stack
  for (int stack = 0; stack < 3; ++stack) {
    uint8_t cols[3] = {0, 1, 2};
    shuffle3(cols);
    // Apply column permutation
    uint8_t col0 = stack * 3 + cols[0];
    uint8_t col1 = stack * 3 + cols[1];
    uint8_t col2 = stack * 3 + cols[2];
    for (int r = 0; r < GRID; ++r) {
      uint8_t v0 = grid[r][col0];
      uint8_t v1 = grid[r][col1];
      uint8_t v2 = grid[r][col2];
      grid[r][stack * 3 + 0] = v0;
      grid[r][stack * 3 + 1] = v1;
      grid[r][stack * 3 + 2] = v2;
    }
  }

  // Shuffle stacks
  uint8_t stacks[3] = {0, 1, 2};
  shuffle3(stacks);
  uint8_t tmpGS[GRID][GRID];
  for (int s = 0; s < 3; ++s) {
    for (int c = 0; c < 3; ++c) {
      for (int r = 0; r < GRID; ++r) {
        tmpGS[r][s * 3 + c] = grid[r][stacks[s] * 3 + c];
      }
    }
  }
  memcpy(grid, tmpGS, sizeof(tmpGS));

  // Relabel digits: apply random permutation of 1-9
  uint8_t labels[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  for (int i = 8; i > 0; --i) {
    int j = rand() % (i + 1);
    uint8_t tmp = labels[i];
    labels[i] = labels[j];
    labels[j] = tmp;
  }
  for (int r = 0; r < GRID; ++r) {
    for (int c = 0; c < GRID; ++c) {
      grid[r][c] = labels[grid[r][c] - 1];
    }
  }
}

void SudokuActivity::removeCells(uint8_t grid[GRID][GRID], int clues) {
  // Build list of all 81 positions and shuffle
  uint8_t positions[81];
  for (int i = 0; i < 81; ++i) positions[i] = i;
  for (int i = 80; i > 0; --i) {
    int j = rand() % (i + 1);
    uint8_t tmp = positions[i];
    positions[i] = positions[j];
    positions[j] = tmp;
  }

  int toRemove = 81 - clues;
  for (int i = 0; i < toRemove && i < 81; ++i) {
    int r = positions[i] / 9;
    int c = positions[i] % 9;
    grid[r][c] = 0;
  }
}

void SudokuActivity::generatePuzzle() {
  srand(millis());

  // Start from the base grid and apply permutations
  uint8_t solution[GRID][GRID];
  memcpy(solution, BASE_GRID, sizeof(BASE_GRID));
  applyPermutation(solution);

  // Copy solution into board, then remove cells to make puzzle
  uint8_t puzzle[GRID][GRID];
  memcpy(puzzle, solution, sizeof(solution));
  removeCells(puzzle, CLUE_COUNT[static_cast<int>(difficulty)]);

  // Fill activity state
  for (int r = 0; r < GRID; ++r) {
    for (int c = 0; c < GRID; ++c) {
      board[r][c] = puzzle[r][c];
      given[r][c] = (puzzle[r][c] != 0);
      error[r][c] = false;
    }
  }

  undoCount = 0;
  cursorRow = 0;
  cursorCol = 0;
  // Advance cursor to first non-given cell
  for (int i = 0; i < GRID * GRID; ++i) {
    int r = i / GRID, c = i % GRID;
    if (!given[r][c]) {
      cursorRow = r;
      cursorCol = c;
      break;
    }
  }
}

void SudokuActivity::checkErrors() {
  for (int r = 0; r < GRID; ++r) {
    for (int c = 0; c < GRID; ++c) {
      if (board[r][c] == 0) {
        error[r][c] = false;
        continue;
      }
      uint8_t v = board[r][c];
      bool bad = false;
      // Check row
      for (int cc = 0; cc < GRID && !bad; ++cc) {
        if (cc != c && board[r][cc] == v) bad = true;
      }
      // Check col
      for (int rr = 0; rr < GRID && !bad; ++rr) {
        if (rr != r && board[rr][c] == v) bad = true;
      }
      // Check box
      int boxR = (r / 3) * 3, boxC = (c / 3) * 3;
      for (int dr = 0; dr < 3 && !bad; ++dr) {
        for (int dc = 0; dc < 3 && !bad; ++dc) {
          int rr = boxR + dr, cc = boxC + dc;
          if ((rr != r || cc != c) && board[rr][cc] == v) bad = true;
        }
      }
      error[r][c] = bad;
    }
  }
}

bool SudokuActivity::isSolved() const {
  for (int r = 0; r < GRID; ++r) {
    for (int c = 0; c < GRID; ++c) {
      if (board[r][c] == 0 || error[r][c]) return false;
    }
  }
  return true;
}

void SudokuActivity::placeNumber(uint8_t row, uint8_t col, uint8_t num) {
  if (given[row][col]) return;
  if (undoCount < UNDO_LIMIT) {
    undoStack[undoCount++] = {row, col, board[row][col], num};
  }
  board[row][col] = num;
  checkErrors();
}

void SudokuActivity::undoMove() {
  if (undoCount == 0) return;
  const Move& m = undoStack[--undoCount];
  board[m.row][m.col] = m.oldVal;
  cursorRow = m.row;
  cursorCol = m.col;
  checkErrors();
}

void SudokuActivity::saveGame() const {
  JsonDocument doc;
  doc["difficulty"] = static_cast<int>(difficulty);
  doc["cursorRow"] = cursorRow;
  doc["cursorCol"] = cursorCol;
  doc["elapsedSeconds"] = elapsedSeconds();

  JsonArray boardArr = doc["board"].to<JsonArray>();
  for (int r = 0; r < GRID; ++r) {
    JsonArray row = boardArr.add<JsonArray>();
    for (int c = 0; c < GRID; ++c) row.add(board[r][c]);
  }
  JsonArray givenArr = doc["given"].to<JsonArray>();
  for (int r = 0; r < GRID; ++r) {
    JsonArray row = givenArr.add<JsonArray>();
    for (int c = 0; c < GRID; ++c) row.add(given[r][c] ? 1 : 0);
  }

  String json;
  serializeJson(doc, json);
  Storage.mkdir("/sudoku", true);
  if (!Storage.writeFile(SAVE_PATH, json)) {
    LOG_ERR("SUDOKU", "Failed to save game");
  }
}

bool SudokuActivity::loadGame() {
  String raw = Storage.readFile(SAVE_PATH);
  if (raw.isEmpty()) return false;

  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) {
    LOG_ERR("SUDOKU", "Failed to parse save");
    return false;
  }

  difficulty = static_cast<Difficulty>(doc["difficulty"] | 0);
  cursorRow = doc["cursorRow"] | 0;
  cursorCol = doc["cursorCol"] | 0;
  pausedElapsed = doc["elapsedSeconds"] | 0;

  JsonArray boardArr = doc["board"].as<JsonArray>();
  JsonArray givenArr = doc["given"].as<JsonArray>();
  if (!boardArr || !givenArr) return false;

  for (int r = 0; r < GRID; ++r) {
    for (int c = 0; c < GRID; ++c) {
      board[r][c] = boardArr[r][c] | 0;
      given[r][c] = (givenArr[r][c] | 0) != 0;
      error[r][c] = false;
    }
  }
  checkErrors();
  undoCount = 0;
  return true;
}

void SudokuActivity::onEnter() {
  Activity::onEnter();
  state = State::SELECT_DIFFICULTY;
  // Try loading a saved game - if present, start in PLAY immediately
  if (loadGame()) {
    state = State::PLAY;
    startTime = millis();
  }
  requestUpdate();
}

void SudokuActivity::onExit() {
  if (state == State::PLAY || state == State::SELECT_NUMBER) {
    pausedElapsed = elapsedSeconds();
    saveGame();
  }
  Activity::onExit();
}

void SudokuActivity::loop() {
  static constexpr int DIFF_COUNT = 4;

  switch (state) {
    case State::SELECT_DIFFICULTY: {
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        // Delete save if starting fresh selection and no game in progress
        onGoHome();
        return;
      }
      // Down/Right = next difficulty
      if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
          mappedInput.wasReleased(MappedInputManager::Button::Right)) {
        difficulty = static_cast<Difficulty>((static_cast<int>(difficulty) + 1) % DIFF_COUNT);
        requestUpdate();
      }
      // Up/Left = previous difficulty
      if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
          mappedInput.wasReleased(MappedInputManager::Button::Left)) {
        difficulty = static_cast<Difficulty>((static_cast<int>(difficulty) + DIFF_COUNT - 1) % DIFF_COUNT);
        requestUpdate();
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        generatePuzzle();
        pausedElapsed = 0;
        startTime = millis();
        state = State::PLAY;
        requestUpdate();
      }
      break;
    }

    case State::PLAY: {
      if (!longPressFired && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
          mappedInput.getHeldTime() >= LONG_PRESS_MS) {
        longPressFired = true;
        selectedNumber = 1;
        state = State::SELECT_NUMBER;
        requestUpdate();
        return;
      }
      if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) longPressFired = false;

      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        pausedElapsed = elapsedSeconds();
        saveGame();
        state = State::SELECT_DIFFICULTY;
        requestUpdate();
        return;
      }

      // Undo: Up + long press (hold Up)
      if (mappedInput.isPressed(MappedInputManager::Button::Up) && mappedInput.getHeldTime() >= LONG_PRESS_MS) {
        undoMove();
        requestUpdate();
        return;
      }

      // Navigate cursor
      if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
          mappedInput.wasReleased(MappedInputManager::Button::Right)) {
        int pos = cursorRow * GRID + cursorCol;
        pos = (pos + 1) % (GRID * GRID);
        cursorRow = pos / GRID;
        cursorCol = pos % GRID;
        requestUpdate();
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
          mappedInput.wasReleased(MappedInputManager::Button::Left)) {
        int pos = cursorRow * GRID + cursorCol;
        pos = (pos + GRID * GRID - 1) % (GRID * GRID);
        cursorRow = pos / GRID;
        cursorCol = pos % GRID;
        requestUpdate();
      }

      // Update timer display every second
      static uint32_t lastSecond = 0;
      uint32_t now = millis();
      if (now - lastSecond >= 1000) {
        lastSecond = now;
        requestUpdate();
      }
      break;
    }

    case State::SELECT_NUMBER: {
      if (!longPressFired && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
          mappedInput.getHeldTime() >= LONG_PRESS_MS) {
        longPressFired = true;
        // Long press confirm = place number
        placeNumber(cursorRow, cursorCol, selectedNumber);
        state = State::PLAY;
        if (isSolved()) {
          pausedElapsed = elapsedSeconds();
          Storage.remove(SAVE_PATH);
          state = State::COMPLETE;
        }
        requestUpdate();
        return;
      }
      if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) longPressFired = false;

      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        state = State::PLAY;
        requestUpdate();
        return;
      }

      // Short press Down/Right = cycle number 1→2→...→9→0(clear)→1
      if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
          mappedInput.wasReleased(MappedInputManager::Button::Right)) {
        selectedNumber = (selectedNumber % 10) + 1;
        if (selectedNumber > 9) selectedNumber = 0;
        // After 0, wrap to 1 (only one clear option)
        requestUpdate();
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
          mappedInput.wasReleased(MappedInputManager::Button::Left)) {
        selectedNumber = selectedNumber - 1;
        if (selectedNumber < 0) selectedNumber = 9;
        requestUpdate();
      }
      break;
    }

    case State::COMPLETE: {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
          mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        state = State::SELECT_DIFFICULTY;
        requestUpdate();
      }
      break;
    }
  }
}

void SudokuActivity::renderGrid(int gridX, int gridY, int cellW, int cellH, int thinLine, int thickLine) {
  const int gridW = GRID * cellW;
  const int gridH = GRID * cellH;

  // Fill cursor cell
  if (state == State::PLAY || state == State::SELECT_NUMBER) {
    renderer.fillRect(gridX + cursorCol * cellW + thinLine, gridY + cursorRow * cellH + thinLine,
                      cellW - thinLine, cellH - thinLine, false);  // invert (gray-ish)
  }

  // Draw cell numbers
  const int numFontId = UI_10_FONT_ID;
  const int lineH = renderer.getLineHeight(numFontId);
  for (int r = 0; r < GRID; ++r) {
    for (int c = 0; c < GRID; ++c) {
      if (board[r][c] == 0) continue;
      char numBuf[2] = {static_cast<char>('0' + board[r][c]), '\0'};
      int numW = renderer.getTextWidth(numFontId, numBuf);
      int tx = gridX + c * cellW + (cellW - numW) / 2;
      int ty = gridY + r * cellH + (cellH - lineH) / 2;
      bool isBlack = true;
      if (r == cursorRow && c == cursorCol && (state == State::PLAY || state == State::SELECT_NUMBER)) {
        isBlack = false;  // white on dark cursor
      }
      if (error[r][c]) {
        // Draw error marker — small dot below number
        renderer.fillRect(tx + numW / 2 - 1, ty + lineH + 1, 3, 3, isBlack);
      }
      EpdFontFamily::Style style = given[r][c] ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      renderer.drawText(numFontId, tx, ty, numBuf, isBlack, style);
    }
  }

  // Draw thin cell lines
  for (int i = 0; i <= GRID; ++i) {
    int lw = ((i % 3) == 0) ? thickLine : thinLine;
    renderer.drawLine(gridX, gridY + i * cellH, gridX + gridW, gridY + i * cellH, lw, true);
    renderer.drawLine(gridX + i * cellW, gridY, gridX + i * cellW, gridY + gridH, lw, true);
  }

  // Selected number indicator (number-select mode)
  if (state == State::SELECT_NUMBER) {
    char selBuf[8];
    if (selectedNumber == 0) {
      snprintf(selBuf, sizeof(selBuf), "[CLR]");
    } else {
      snprintf(selBuf, sizeof(selBuf), "[  %d  ]", selectedNumber);
    }
    int selW = renderer.getTextWidth(UI_12_FONT_ID, selBuf);
    int selX = gridX + gridW / 2 - selW / 2;
    int selY = gridY + gridH + 8;
    renderer.drawText(UI_12_FONT_ID, selX, selY, selBuf, true, EpdFontFamily::BOLD);
  }
}

void SudokuActivity::render(RenderLock&&) {
  renderer.clearScreen();
  switch (state) {
    case State::SELECT_DIFFICULTY:
      renderSelectDifficulty();
      break;
    case State::PLAY:
    case State::SELECT_NUMBER:
      renderPlay();
      break;
    case State::COMPLETE:
      renderComplete();
      break;
  }
  renderer.displayBuffer();
}

void SudokuActivity::renderSelectDifficulty() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SUDOKU));

  // If a saved game exists, mention it
  const int centerY = (pageHeight - metrics.headerHeight - metrics.buttonHintsHeight) / 2 + metrics.headerHeight;
  renderer.drawCenteredText(UI_12_FONT_ID, centerY - 30, tr(STR_SUDOKU_SELECT_DIFF), true, EpdFontFamily::REGULAR);

  // Difficulty name with arrows
  char diffBuf[32];
  snprintf(diffBuf, sizeof(diffBuf), "< %s >", difficultyName(difficulty));
  renderer.drawCenteredText(UI_12_FONT_ID, centerY, diffBuf, true, EpdFontFamily::BOLD);

  // Clue count hint
  char clueBuf[32];
  snprintf(clueBuf, sizeof(clueBuf), "(%d clues)", CLUE_COUNT[static_cast<int>(difficulty)]);
  renderer.drawCenteredText(UI_10_FONT_ID, centerY + 30, clueBuf, true, EpdFontFamily::REGULAR);

  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_SUDOKU_START), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void SudokuActivity::renderPlay() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Timer string in header subtitle
  uint32_t sec = elapsedSeconds();
  char timerBuf[12];
  snprintf(timerBuf, sizeof(timerBuf), "%02lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SUDOKU), timerBuf);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int hintH = metrics.buttonHintsHeight + metrics.verticalSpacing;
  const int availH = pageHeight - contentTop - hintH;
  const int availW = pageWidth;

  // Compute cell size to fit grid in available space (square cells)
  int cellSize = std::min(availW / GRID, availH / GRID);
  const int gridW = GRID * cellSize;
  const int gridH = GRID * cellSize;
  const int gridX = (pageWidth - gridW) / 2;
  const int gridY = contentTop + (availH - gridH) / 2;

  renderGrid(gridX, gridY, cellSize, cellSize, 1, 2);

  const char* confirmLabel = (state == State::SELECT_NUMBER) ? tr(STR_SUDOKU_PLACE) : tr(STR_SUDOKU_PICK);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void SudokuActivity::renderComplete() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SUDOKU));

  const int centerY = (pageHeight - metrics.headerHeight - metrics.buttonHintsHeight) / 2 + metrics.headerHeight;
  renderer.drawCenteredText(UI_12_FONT_ID, centerY - 20, tr(STR_SUDOKU_COMPLETE), true, EpdFontFamily::BOLD);

  uint32_t sec = pausedElapsed;
  char timeBuf[32];
  snprintf(timeBuf, sizeof(timeBuf), "%02lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
  renderer.drawCenteredText(UI_10_FONT_ID, centerY + 16, timeBuf, true, EpdFontFamily::REGULAR);

  const auto labels = mappedInput.mapLabels("", tr(STR_OK_BUTTON), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
