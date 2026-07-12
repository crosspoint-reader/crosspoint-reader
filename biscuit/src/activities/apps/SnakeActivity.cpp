#include "SnakeActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <esp_random.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

// 25% gray (light) — every other pixel in checkerboard on even rows only
static void fillDithered25(GfxRenderer& r, int x, int y, int w, int h) {
  for (int dy = 0; dy < h; dy += 2)
    for (int dx = ((dy / 2) % 2); dx < w; dx += 2)
      r.drawPixel(x + dx, y + dy, true);
}

void SnakeActivity::onEnter() {
  Activity::onEnter();
  initGame();
}

void SnakeActivity::onExit() { Activity::onExit(); }

void SnakeActivity::initGame() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int screenW = renderer.getScreenWidth();
  int screenH = renderer.getScreenHeight();

  // Reserve top area for score and bottom for button hints
  int topReserve = metrics.topPadding + 25;
  int bottomReserve = metrics.buttonHintsHeight;

  gridW = screenW / CELL_SIZE;
  gridH = (screenH - topReserve - bottomReserve) / CELL_SIZE;
  offsetX = (screenW - gridW * CELL_SIZE) / 2;
  offsetY = topReserve;

  snake.clear();
  int startX = gridW / 2;
  int startY = gridH / 2;
  snake.push_back({startX, startY});
  snake.push_back({startX - 1, startY});
  snake.push_back({startX - 2, startY});

  dirX = 1;
  dirY = 0;
  nextDirX = 1;
  nextDirY = 0;
  score = 0;
  state = PLAYING;
  lastStepMs = millis();

  spawnFood();
  requestUpdate();
}

void SnakeActivity::spawnFood() {
  int attempts = 0;
  do {
    food.x = esp_random() % gridW;
    food.y = esp_random() % gridH;
    attempts++;
  } while (isSnakeAt(food.x, food.y) && attempts < 1000);
}

bool SnakeActivity::isSnakeAt(int x, int y) const {
  for (auto& seg : snake) {
    if (seg.x == x && seg.y == y) return true;
  }
  return false;
}

void SnakeActivity::step() {
  // Apply buffered direction
  dirX = nextDirX;
  dirY = nextDirY;

  Point head = snake.front();
  Point newHead = {head.x + dirX, head.y + dirY};

  // Wall collision
  if (newHead.x < 0 || newHead.x >= gridW || newHead.y < 0 || newHead.y >= gridH) {
    state = GAME_OVER;
    requestUpdate();
    return;
  }

  // Self collision
  if (isSnakeAt(newHead.x, newHead.y)) {
    state = GAME_OVER;
    requestUpdate();
    return;
  }

  snake.insert(snake.begin(), newHead);

  // Check food
  if (newHead.x == food.x && newHead.y == food.y) {
    score += 10;
    spawnFood();
  } else {
    snake.pop_back();
  }

  requestUpdate();
}

void SnakeActivity::loop() {
  if (state == GAME_OVER) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      initGame();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  // Direction input - prevent reversal
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) && dirY == 0) {
    nextDirX = 0;
    nextDirY = -1;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down) && dirY == 0) {
    nextDirX = 0;
    nextDirY = 1;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Left) && dirX == 0) {
    nextDirX = -1;
    nextDirY = 0;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Right) && dirX == 0) {
    nextDirX = 1;
    nextDirY = 0;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Step timer
  unsigned long now = millis();
  if (now - lastStepMs >= STEP_INTERVAL_MS) {
    lastStepMs = now;
    step();
  }
}

void SnakeActivity::render(RenderLock&&) {
  renderer.clearScreen();

  switch (state) {
    case PLAYING:
      renderPlaying();
      break;
    case GAME_OVER:
      renderGameOver();
      break;
  }

  renderer.displayBuffer();
}

void SnakeActivity::renderPlaying() const {
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Double-line border
  renderer.drawRect(offsetX - 1, offsetY - 1, gridW * CELL_SIZE + 2, gridH * CELL_SIZE + 2);
  renderer.drawRect(offsetX - 3, offsetY - 3, gridW * CELL_SIZE + 6, gridH * CELL_SIZE + 6);

  // Subtle background texture
  fillDithered25(renderer, offsetX, offsetY, gridW * CELL_SIZE, gridH * CELL_SIZE);

  // Snake body (with 1px white gap between segments for articulated look)
  for (size_t i = 0; i < snake.size(); i++) {
    int px = offsetX + snake[i].x * CELL_SIZE;
    int py = offsetY + snake[i].y * CELL_SIZE;
    if (i == 0) {
      // Head — filled with eyes
      renderer.fillRect(px + 1, py + 1, CELL_SIZE - 2, CELL_SIZE - 2);
      // Eyes based on direction
      if (dirX == 1) { // right
        renderer.drawPixel(px + CELL_SIZE - 3, py + 2, false);
        renderer.drawPixel(px + CELL_SIZE - 3, py + CELL_SIZE - 3, false);
      } else if (dirX == -1) { // left
        renderer.drawPixel(px + 2, py + 2, false);
        renderer.drawPixel(px + 2, py + CELL_SIZE - 3, false);
      } else if (dirY == -1) { // up
        renderer.drawPixel(px + 2, py + 2, false);
        renderer.drawPixel(px + CELL_SIZE - 3, py + 2, false);
      } else { // down
        renderer.drawPixel(px + 2, py + CELL_SIZE - 3, false);
        renderer.drawPixel(px + CELL_SIZE - 3, py + CELL_SIZE - 3, false);
      }
    } else {
      // Body segment with 1px gap (draw slightly smaller)
      renderer.fillRect(px + 2, py + 2, CELL_SIZE - 4, CELL_SIZE - 4);
    }
  }

  // Food — apple shape
  {
    int px = offsetX + food.x * CELL_SIZE;
    int py = offsetY + food.y * CELL_SIZE;
    int cx = px + CELL_SIZE / 2;
    int cy = py + CELL_SIZE / 2 + 1;
    int r = CELL_SIZE / 3;
    // Filled circle body
    for (int dy = -r; dy <= r; dy++) {
      int dx = 0;
      while ((dx + 1) * (dx + 1) + dy * dy <= r * r) dx++;
      if (dx > 0) renderer.fillRect(cx - dx, cy + dy, dx * 2 + 1, 1, true);
      else renderer.drawPixel(cx, cy + dy, true);
    }
    // Stem on top
    renderer.fillRect(cx, cy - r - 2, 2, 3, true);
  }

  // Score (drawn below the grid)
  int scoreY = offsetY + gridH * CELL_SIZE + 10;
  char scoreBuf[48];
  snprintf(scoreBuf, sizeof(scoreBuf), "Score: %d  Length: %d", score, (int)snake.size());
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, scoreY, scoreBuf, true, EpdFontFamily::BOLD);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void SnakeActivity::renderGameOver() const {
  const auto pageHeight = renderer.getScreenHeight();
  int y = pageHeight / 2 - 40;

  renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_GAME_OVER), true, EpdFontFamily::BOLD);
  y += 40;

  char scoreBuf[32];
  snprintf(scoreBuf, sizeof(scoreBuf), "Score: %d", score);
  renderer.drawCenteredText(UI_10_FONT_ID, y, scoreBuf);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
