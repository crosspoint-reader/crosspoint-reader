#include "GameOfLifeActivity.h"

#include <I18n.h>
#include <esp_random.h>

#include <cstring>
#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"

// Pattern data — packed bits, row-major, MSB = leftmost pixel
static constexpr uint8_t PAT_BLINKER[] = {0xE0};  // ###
static constexpr uint8_t PAT_BLOCK[] = {0xC0, 0xC0};  // ## / ##
static constexpr uint8_t PAT_BEEHIVE[] = {0x60, 0x90, 0x60};  // .##. / #..# / .##.
static constexpr uint8_t PAT_TOAD[] = {0x70, 0xE0};  // .### / ###.
static constexpr uint8_t PAT_BEACON[] = {0xC0, 0xC0, 0x30, 0x30};  // ##.. / ##.. / ..## / ..##
static constexpr uint8_t PAT_GLIDER[] = {0x40, 0x20, 0xE0};  // .#. / ..# / ###
static constexpr uint8_t PAT_LWSS[] = {0x48, 0x80, 0x88, 0xF0};  // .#..# / #.... / #...# / ####.
static constexpr uint8_t PAT_RPENTOMINO[] = {0x60, 0xC0, 0x40};  // .## / ##. / .#.
static constexpr uint8_t PAT_DIEHARD[] = {0x02, 0xC0, 0x47};  // ......#. / ##...... / .#...###
static constexpr uint8_t PAT_ACORN[] = {0x40, 0x10, 0xCE};  // .#..... / ...#... / ##..###

// Pulsar (13x13) — 2 bytes per row
static constexpr uint8_t PAT_PULSAR[] = {
  0x0E, 0x38,  // ..###...###..
  0x00, 0x00,  // .............
  0x42, 0x10,  // #....#.#....#
  0x42, 0x10,  // #....#.#....#
  0x42, 0x10,  // #....#.#....#
  0x0E, 0x38,  // ..###...###..
  0x00, 0x00,  // .............
  0x0E, 0x38,  // ..###...###..
  0x42, 0x10,  // #....#.#....#
  0x42, 0x10,  // #....#.#....#
  0x42, 0x10,  // #....#.#....#
  0x00, 0x00,  // .............
  0x0E, 0x38,  // ..###...###..
};

// Gosper Glider Gun (36x9) — 5 bytes per row
static constexpr uint8_t PAT_GOSPER_GUN[] = {
  0x00, 0x00, 0x01, 0x00, 0x00,
  0x00, 0x00, 0x05, 0x00, 0x00,
  0x00, 0x06, 0x06, 0x00, 0x18,
  0x00, 0x08, 0xA6, 0x00, 0x18,
  0x60, 0x10, 0x46, 0x00, 0x00,
  0x60, 0x10, 0xA2, 0x80, 0x00,
  0x00, 0x10, 0x40, 0x80, 0x00,
  0x00, 0x08, 0x80, 0x00, 0x00,
  0x00, 0x06, 0x00, 0x00, 0x00,
};

const GameOfLifeActivity::PatternDef GameOfLifeActivity::PATTERNS[] = {
  {"Glider", "Moves diagonally forever", 3, 3, PAT_GLIDER},
  {"LWSS", "Lightweight Spaceship", 5, 4, PAT_LWSS},
  {"R-pentomino", "Methuselah - 1103 gens", 3, 3, PAT_RPENTOMINO},
  {"Acorn", "Methuselah - 5206 gens", 7, 3, PAT_ACORN},
  {"Diehard", "Vanishes after 130 gens", 8, 3, PAT_DIEHARD},
  {"Gosper Gun", "Infinite glider stream", 36, 9, PAT_GOSPER_GUN},
  {"Pulsar", "Period-3 oscillator", 13, 13, PAT_PULSAR},
  {"Beacon", "Period-2 oscillator", 4, 4, PAT_BEACON},
  {"Blinker", "Simplest oscillator", 3, 1, PAT_BLINKER},
  {"Toad", "Period-2 oscillator", 4, 2, PAT_TOAD},
  {"Block", "Still life", 2, 2, PAT_BLOCK},
  {"Beehive", "Still life", 4, 3, PAT_BEEHIVE},
};

void GameOfLifeActivity::onEnter() {
  Activity::onEnter();
  running = false;
  generation = 0;
  extState = RUNNING_SIM;
  patternIndex = 0;
  randomize();
  requestUpdate();
}

void GameOfLifeActivity::randomize() {
  for (int x = 0; x < COLS; x++) {
    for (int y = 0; y < ROWS; y++) {
      grid[x][y] = (esp_random() % 4 == 0) ? 1 : 0;
    }
  }
  generation = 0;
  population = countPopulation();
  memset(popHistory, 0, sizeof(popHistory));
  popHistoryIdx = 0;
  popHistoryMax = 1;
}

int GameOfLifeActivity::countNeighbors(int x, int y) const {
  int count = 0;
  for (int dx = -1; dx <= 1; dx++) {
    for (int dy = -1; dy <= 1; dy++) {
      if (dx == 0 && dy == 0) continue;
      int nx = (x + dx + COLS) % COLS;
      int ny = (y + dy + ROWS) % ROWS;
      count += grid[nx][ny];
    }
  }
  return count;
}

void GameOfLifeActivity::step() {
  for (int x = 0; x < COLS; x++) {
    for (int y = 0; y < ROWS; y++) {
      int n = countNeighbors(x, y);
      if (grid[x][y]) {
        nextGrid[x][y] = (n == 2 || n == 3) ? 1 : 0;
      } else {
        nextGrid[x][y] = (n == 3) ? 1 : 0;
      }
    }
  }
  memcpy(grid, nextGrid, sizeof(grid));
  generation++;
  population = countPopulation();
  popHistory[popHistoryIdx] = population;
  popHistoryIdx = (popHistoryIdx + 1) % POP_HISTORY_SIZE;
  // Update max for graph scaling
  popHistoryMax = 1;
  for (int i = 0; i < POP_HISTORY_SIZE; i++) {
    if (popHistory[i] > popHistoryMax) popHistoryMax = popHistory[i];
  }
}

int GameOfLifeActivity::countPopulation() const {
  int count = 0;
  for (int x = 0; x < COLS; x++) {
    for (int y = 0; y < ROWS; y++) {
      count += grid[x][y];
    }
  }
  return count;
}

void GameOfLifeActivity::loadPattern(int index) {
  if (index < 0 || index >= PATTERN_COUNT) return;
  const auto& pat = PATTERNS[index];

  memset(grid, 0, sizeof(grid));
  generation = 0;
  running = false;
  memset(popHistory, 0, sizeof(popHistory));
  popHistoryIdx = 0;
  popHistoryMax = 1;

  int ox = (COLS - pat.width) / 2;
  int oy = (ROWS - pat.height) / 2;
  placePatternAt(pat, ox, oy);
  population = countPopulation();
}

void GameOfLifeActivity::placePatternAt(const PatternDef& pat, int ox, int oy) {
  int bytesPerRow = (pat.width + 7) / 8;
  for (int y = 0; y < pat.height; y++) {
    for (int x = 0; x < pat.width; x++) {
      int byteIdx = y * bytesPerRow + x / 8;
      int bitIdx = 7 - (x % 8);
      if ((pat.data[byteIdx] >> bitIdx) & 1) {
        int gx = ox + x;
        int gy = oy + y;
        if (gx >= 0 && gx < COLS && gy >= 0 && gy < ROWS) {
          grid[gx][gy] = 1;
        }
      }
    }
  }
}

void GameOfLifeActivity::loop() {
  if (extState == RUNNING_SIM) {
    if (running) {
      unsigned long now = millis();
      if (now - lastStepTime >= STEP_INTERVAL_MS) {
        lastStepTime = now;
        step();
        requestUpdate();
      }
    }

    // Toggle run/pause
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (running) {
        running = false;
      } else {
        running = true;
        lastStepTime = millis();
      }
      requestUpdate();
    }

    // Single step (when paused)
    if (!running && mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      step();
      requestUpdate();
    }

    // Randomize
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      running = false;
      randomize();
      requestUpdate();
    }

    // Toggle cell at cursor (when paused)
    if (!running) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
        if (cursorY > 0) cursorY--;
        requestUpdate();
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
        if (cursorY < ROWS - 1) cursorY++;
        requestUpdate();
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
  }

  // Pattern library — opened via PageForward side button
  if (extState == RUNNING_SIM) {
    if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
      running = false;
      extState = PATTERN_SELECT;
      requestUpdate();
    }
  }

  if (extState == PATTERN_SELECT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      extState = RUNNING_SIM;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      loadPattern(patternIndex);
      extState = RUNNING_SIM;
      requestUpdate();
    }
    buttonNavigator.onNext([this] { patternIndex = ButtonNavigator::nextIndex(patternIndex, PATTERN_COUNT); requestUpdate(); });
    buttonNavigator.onPrevious([this] { patternIndex = ButtonNavigator::previousIndex(patternIndex, PATTERN_COUNT); requestUpdate(); });
  }
}

void GameOfLifeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  if (extState == PATTERN_SELECT) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Pattern Library");

    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

    GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, PATTERN_COUNT, patternIndex,
        [](int i) -> std::string { return GameOfLifeActivity::PATTERNS[i].name; },
        [](int i) -> std::string { return GameOfLifeActivity::PATTERNS[i].description; });

    const auto labels = mappedInput.mapLabels("Cancel", "Load", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    GUI.drawSideButtonHints(renderer, "", "");
    renderer.displayBuffer();
    return;
  }

  // Compact header
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GAME_OF_LIFE));

  const int headerBottom = metrics.topPadding + metrics.headerHeight + 2;
  const int hintsTop = pageHeight - metrics.buttonHintsHeight;

  // Reserve 55px at bottom for info bar + population graph
  static constexpr int GRAPH_H = 40;
  static constexpr int INFO_BAR_H = 15;
  const int graphBottom = hintsTop - 4;
  const int graphTop = graphBottom - GRAPH_H;
  const int infoBarY = graphTop - INFO_BAR_H;

  // Grid fills space between header and info bar
  const int availHeight = infoBarY - headerBottom - 4;
  const int availWidth = pageWidth;

  int cellW = availWidth / COLS;
  int cellH = availHeight / ROWS;
  int cellSize = std::min(cellW, cellH);
  if (cellSize < 2) cellSize = 2;

  const int gridWidth = cellSize * COLS;
  const int gridHeight = cellSize * ROWS;
  const int gridX = (pageWidth - gridWidth) / 2;
  const int gridY = headerBottom + (availHeight - gridHeight) / 2;

  // Draw thin border around grid
  renderer.drawRect(gridX - 1, gridY - 1, gridWidth + 2, gridHeight + 2, true);

  // Draw alive cells
  for (int x = 0; x < COLS; x++) {
    for (int y = 0; y < ROWS; y++) {
      if (grid[x][y]) {
        int px = gridX + x * cellSize;
        int py = gridY + y * cellSize;
        renderer.fillRect(px, py, cellSize, cellSize, true);
      }
    }
  }

  // Info bar: generation + population in compact format
  char infoBuf[64];
  snprintf(infoBuf, sizeof(infoBuf), "Gen: %d   Pop: %d   %s",
           generation, population, running ? "RUNNING" : "PAUSED");
  renderer.drawCenteredText(SMALL_FONT_ID, infoBarY, infoBuf);

  // Separator line
  renderer.drawLine(15, graphTop - 2, pageWidth - 15, graphTop - 2);

  // Population graph — line graph of last 40 values
  int graphW = pageWidth - 40;
  int graphX = 20;

  // Draw graph border
  renderer.drawRect(graphX, graphTop, graphW, GRAPH_H, true);

  // Plot population history as line graph
  if (popHistoryMax > 0) {
    int prevScreenX = -1, prevScreenY = -1;
    for (int i = 0; i < POP_HISTORY_SIZE; i++) {
      // Read history in order (oldest first)
      int idx = (popHistoryIdx + i) % POP_HISTORY_SIZE;
      int val = popHistory[idx];
      int screenX = graphX + 1 + i * (graphW - 2) / POP_HISTORY_SIZE;
      int screenY = graphTop + GRAPH_H - 2 - val * (GRAPH_H - 4) / popHistoryMax;
      if (screenY < graphTop + 1) screenY = graphTop + 1;
      if (prevScreenX >= 0 && val > 0) {
        renderer.drawLine(prevScreenX, prevScreenY, screenX, screenY);
      }
      prevScreenX = screenX;
      prevScreenY = screenY;
    }
  }

  // Button hints
  if (running) {
    const auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_PAUSED), tr(STR_RANDOMIZE), "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    const auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_RUNNING), tr(STR_RANDOMIZE), tr(STR_STEP));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  GUI.drawSideButtonHints(renderer, "", "Patterns");

  renderer.displayBuffer();
}
