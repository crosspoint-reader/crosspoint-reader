#include "DiceRollerActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <esp_random.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

// 25% gray (light dither) for die face texture
static void fillDithered25(GfxRenderer& r, int x, int y, int w, int h) {
  for (int dy = 0; dy < h; dy += 2)
    for (int dx = ((dy/2) % 2); dx < w; dx += 2)
      r.drawPixel(x + dx, y + dy, true);
}

// 50% gray (medium) — for shadow dithering
static void fillDithered50(GfxRenderer& r, int x, int y, int w, int h) {
  for (int dy = 0; dy < h; dy++)
    for (int dx = (dy % 2); dx < w; dx += 2)
      r.drawPixel(x + dx, y + dy, true);
}

constexpr int DiceRollerActivity::DIE_TYPES[];

uint32_t DiceRollerActivity::randomRange(uint32_t max) { return (esp_random() % max) + 1; }

void DiceRollerActivity::onEnter() {
  Activity::onEnter();
  state = SELECT;
  dieTypeIndex = 1;
  dieCount = 1;
  diceResults.clear();
  total = 0;
  requestUpdate();
}

void DiceRollerActivity::onExit() { Activity::onExit(); }

void DiceRollerActivity::doRoll() {
  diceResults.clear();
  total = 0;
  int sides = DIE_TYPES[dieTypeIndex];
  for (int i = 0; i < dieCount; i++) {
    int val = static_cast<int>(randomRange(sides));
    diceResults.push_back(val);
    total += val;
  }
}

void DiceRollerActivity::loop() {
  if (state == SELECT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) { finish(); return; }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      dieTypeIndex = ButtonNavigator::previousIndex(dieTypeIndex, NUM_DIE_TYPES); requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      dieTypeIndex = ButtonNavigator::nextIndex(dieTypeIndex, NUM_DIE_TYPES); requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (dieCount > 1) { dieCount--; requestUpdate(); }
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (dieCount < 6) { dieCount++; requestUpdate(); }
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      state = ROLLING; animFrame = 0; animStartMs = millis(); requestUpdate();
    }
    return;
  }

  if (state == ROLLING) {
    unsigned long elapsed = millis() - animStartMs;
    int frame = static_cast<int>(elapsed / ANIM_FRAME_MS);
    if (frame >= ANIM_FRAMES) {
      doRoll(); state = RESULT; requestUpdate();
    } else if (frame != animFrame) {
      animFrame = frame;
      diceResults.clear(); total = 0;
      int sides = DIE_TYPES[dieTypeIndex];
      for (int i = 0; i < dieCount; i++) {
        int val = static_cast<int>(randomRange(sides));
        diceResults.push_back(val); total += val;
      }
      requestUpdate();
    }
    return;
  }

  if (state == RESULT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) { state = SELECT; requestUpdate(); return; }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      state = ROLLING; animFrame = 0; animStartMs = millis(); requestUpdate();
    }
  }
}

// ---- visual dice rendering helpers ----

// Draw a single pip (dot) as a filled circle approximation
static void drawPip(GfxRenderer& r, int cx, int cy, int radius) {
  // Draw filled circle using horizontal lines
  for (int dy = -radius; dy <= radius; dy++) {
    int dx = 0;
    // Calculate width at this row: dx² + dy² <= r²
    while ((dx + 1) * (dx + 1) + dy * dy <= radius * radius) dx++;
    if (dx > 0) {
      r.fillRect(cx - dx, cy + dy, dx * 2 + 1, 1, true);
    } else {
      r.drawPixel(cx, cy + dy, true);
    }
  }
}

// Draw d6 pip pattern inside a die face
static void drawD6Pips(GfxRenderer& r, int x, int y, int size, int value) {
  const int pip = size / 7;  // pip radius — slightly larger for visibility
  const int margin = size / 4;
  const int left = x + margin;
  const int right = x + size - margin;
  const int top = y + margin;
  const int bottom = y + size - margin;
  const int midX = x + size / 2;
  const int midY = y + size / 2;

  // Standard d6 pip layouts
  switch (value) {
    case 1:
      drawPip(r, midX, midY, pip);
      break;
    case 2:
      drawPip(r, right, top, pip);
      drawPip(r, left, bottom, pip);
      break;
    case 3:
      drawPip(r, right, top, pip);
      drawPip(r, midX, midY, pip);
      drawPip(r, left, bottom, pip);
      break;
    case 4:
      drawPip(r, left, top, pip);
      drawPip(r, right, top, pip);
      drawPip(r, left, bottom, pip);
      drawPip(r, right, bottom, pip);
      break;
    case 5:
      drawPip(r, left, top, pip);
      drawPip(r, right, top, pip);
      drawPip(r, midX, midY, pip);
      drawPip(r, left, bottom, pip);
      drawPip(r, right, bottom, pip);
      break;
    case 6:
      drawPip(r, left, top, pip);
      drawPip(r, right, top, pip);
      drawPip(r, left, midY, pip);
      drawPip(r, right, midY, pip);
      drawPip(r, left, bottom, pip);
      drawPip(r, right, bottom, pip);
      break;
    default:
      break;
  }
}

// Draw a single die face at given position
static void drawDieFace(GfxRenderer& r, int x, int y, int size, int value, int sides) {
  // Shadow (dithered 50% for more natural look)
  fillDithered50(r, x + 4, y + 4, size, size);
  // White face
  r.fillRect(x, y, size, size, false);
  // Subtle ivory texture
  fillDithered25(r, x + 4, y + 4, size - 8, size - 8);
  // Border (double line for weight)
  r.drawRect(x, y, size, size, true);
  r.drawRect(x + 1, y + 1, size - 2, size - 2, true);
  // Inner border for depth
  r.drawRect(x + 3, y + 3, size - 6, size - 6, true);

  if (sides == 6 && value >= 1 && value <= 6) {
    // Draw proper pips for d6
    drawD6Pips(r, x, y, size, value);
  } else {
    // For non-d6: draw the number centered inside
    // Also draw a small "d-type" label at bottom
    char numBuf[8];
    snprintf(numBuf, sizeof(numBuf), "%d", value);

    // Large number centered
    int tw = r.getTextWidth(UI_12_FONT_ID, numBuf, EpdFontFamily::BOLD);
    int th = r.getTextHeight(UI_12_FONT_ID);
    r.drawText(UI_12_FONT_ID, x + (size - tw) / 2, y + (size - th) / 2 - 5, numBuf, true, EpdFontFamily::BOLD);

    // Small "dN" label at bottom of die
    char typeBuf[8];
    snprintf(typeBuf, sizeof(typeBuf), "d%d", sides);
    int tw2 = r.getTextWidth(SMALL_FONT_ID, typeBuf);
    r.drawText(SMALL_FONT_ID, x + (size - tw2) / 2, y + size - 20, typeBuf);
  }
}

// Compute scattered positions for dice (looks like thrown on table)
struct DiePos { int x, y; };
static void computeScatteredPositions(DiePos* out, int count, int screenW, int screenH,
                                       int dieSize, int headerBottom, const std::vector<int>& values) {
  // Area available for dice
  const int areaTop = headerBottom + 20;
  const int areaH = screenH - areaTop - 140;  // leave room for total at bottom
  const int areaW = screenW - 40;
  const int startX = 20;

  if (count == 1) {
    out[0] = {screenW / 2 - dieSize / 2, areaTop + areaH / 2 - dieSize / 2};
    return;
  }

  // Grid layout with jitter
  int cols, rows;
  if (count <= 2) { cols = 2; rows = 1; }
  else if (count <= 4) { cols = 2; rows = 2; }
  else { cols = 3; rows = 2; }

  int spacingX = areaW / cols;
  int spacingY = areaH / rows;

  for (int i = 0; i < count; i++) {
    int col = i % cols;
    int row = i / cols;
    int baseX = startX + col * spacingX + (spacingX - dieSize) / 2;
    int baseY = areaTop + row * spacingY + (spacingY - dieSize) / 2;

    // Deterministic jitter from die value and index
    int jx = ((values[i] * 7 + i * 13) % 21) - 10;  // -10 to +10
    int jy = ((values[i] * 11 + i * 5) % 17) - 8;   // -8 to +8

    out[i] = {baseX + jx, baseY + jy};
  }
}

void DiceRollerActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DICE_ROLLER));

  switch (state) {
    case SELECT: renderSelect(); break;
    case ROLLING: renderRolling(); break;
    case RESULT: renderResult(); break;
  }
  renderer.displayBuffer();
}

void DiceRollerActivity::renderSelect() const {
  const auto pageHeight = renderer.getScreenHeight();
  int y = pageHeight / 2 - 50;

  renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_SELECT_DICE));
  y += 35;

  char dieBuf[32];
  snprintf(dieBuf, sizeof(dieBuf), "%dd%d", dieCount, DIE_TYPES[dieTypeIndex]);
  renderer.drawCenteredText(UI_12_FONT_ID, y, dieBuf, true, EpdFontFamily::BOLD);
  y += 40;

  renderer.drawCenteredText(SMALL_FONT_ID, y, "Up/Down: die type  Left/Right: count");

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_ROLL), "<", ">");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, "Type", "Type");
}

void DiceRollerActivity::renderRolling() const {
  // During rolling animation, draw dice with random preview values
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int headerBottom = metrics.topPadding + metrics.headerHeight;

  if (diceResults.empty()) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 20, tr(STR_ROLLING), true, EpdFontFamily::BOLD);
    return;
  }

  int sides = DIE_TYPES[dieTypeIndex];
  int count = static_cast<int>(diceResults.size());

  // Size dice based on count
  int dieSize;
  if (count == 1) dieSize = 140;
  else if (count <= 2) dieSize = 120;
  else if (count <= 4) dieSize = 100;
  else dieSize = 85;

  DiePos positions[6];
  computeScatteredPositions(positions, count, pageWidth, pageHeight, dieSize, headerBottom, diceResults);

  // Add bounce jitter during animation
  for (int i = 0; i < count; i++) {
    positions[i].x += ((int)(esp_random() % 11) - 5);
    positions[i].y += ((int)(esp_random() % 11) - 5);
  }

  for (int i = 0; i < count; i++) {
    drawDieFace(renderer, positions[i].x, positions[i].y, dieSize, diceResults[i], sides);
  }
}

void DiceRollerActivity::renderResult() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int headerBottom = metrics.topPadding + metrics.headerHeight;

  int sides = DIE_TYPES[dieTypeIndex];
  int count = static_cast<int>(diceResults.size());

  if (count == 0) return;

  // Size dice based on count
  int dieSize;
  if (count == 1) dieSize = 140;
  else if (count <= 2) dieSize = 120;
  else if (count <= 4) dieSize = 100;
  else dieSize = 85;

  // Draw scattered dice
  DiePos positions[6];
  computeScatteredPositions(positions, count, pageWidth, pageHeight, dieSize, headerBottom, diceResults);

  for (int i = 0; i < count; i++) {
    drawDieFace(renderer, positions[i].x, positions[i].y, dieSize, diceResults[i], sides);
  }

  // Total at the bottom
  int bottomY = pageHeight - metrics.buttonHintsHeight - 70;

  // Die description + result
  char dieBuf[32];
  snprintf(dieBuf, sizeof(dieBuf), "%dd%d", dieCount, sides);

  // Box around the total line for emphasis (drawn before text so text renders on top)
  renderer.drawRect(40, bottomY - 6, pageWidth - 80, 32, true);

  if (dieCount > 1) {
    // Show breakdown: "2d6: 3 + 5 = 8"
    std::string expr = std::string(dieBuf) + ": ";
    for (int i = 0; i < count; i++) {
      if (i > 0) expr += " + ";
      expr += std::to_string(diceResults[i]);
    }
    expr += " = " + std::to_string(total);
    renderer.drawCenteredText(UI_10_FONT_ID, bottomY, expr.c_str(), true, EpdFontFamily::BOLD);
  } else {
    // Single die — just show "d6: 4"
    char buf[32];
    snprintf(buf, sizeof(buf), "%s: %d", dieBuf, total);
    renderer.drawCenteredText(UI_10_FONT_ID, bottomY, buf, true, EpdFontFamily::BOLD);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_REROLL), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
