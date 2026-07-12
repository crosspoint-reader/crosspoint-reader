#include "CalculatorActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

// ----------------------------------------------------------------
// Static data (in flash)
// ----------------------------------------------------------------

const char* const CalculatorActivity::KEY_LABELS[GRID_ROWS][GRID_COLS] = {
  {"C",  "+/-", "%",  "/"},
  {"7",  "8",   "9",  "x"},
  {"4",  "5",   "6",  "-"},
  {"1",  "2",   "3",  "+"},
  {"0",  ".",   "<",  "="},
};

// ----------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------

void CalculatorActivity::fillDithered25(GfxRenderer& r, int x, int y, int w, int h) {
  for (int dy = 0; dy < h; dy += 2)
    for (int dx = ((dy / 2) % 2); dx < w; dx += 2)
      r.drawPixel(x + dx, y + dy, true);
}

// ----------------------------------------------------------------
// Lifecycle
// ----------------------------------------------------------------

void CalculatorActivity::onEnter() {
  Activity::onEnter();
  accumulator    = 0.0;
  currentValue   = 0.0;
  pendingOp      = '\0';
  hasDecimal     = false;
  decimalPlaces  = 0;
  newInput       = true;
  showingResult  = false;
  strncpy(displayStr, "0", sizeof(displayStr));
  displayStr[sizeof(displayStr) - 1] = '\0';
  expressionStr[0] = '\0';
  cursorRow      = 3;
  cursorCol      = 0;
  historyCount   = 0;
  requestUpdate();
}

void CalculatorActivity::onExit() {
  Activity::onExit();
}

// ----------------------------------------------------------------
// Input logic
// ----------------------------------------------------------------

void CalculatorActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  bool changed = false;

  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    cursorRow = (cursorRow == 0) ? (GRID_ROWS - 1) : (cursorRow - 1);
    changed = true;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    cursorRow = (cursorRow == GRID_ROWS - 1) ? 0 : (cursorRow + 1);
    changed = true;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    cursorCol = (cursorCol == 0) ? (GRID_COLS - 1) : (cursorCol - 1);
    changed = true;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    cursorCol = (cursorCol == GRID_COLS - 1) ? 0 : (cursorCol + 1);
    changed = true;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    pressKey(cursorRow, cursorCol);
    changed = true;
  }

  if (changed) requestUpdate();
}

void CalculatorActivity::pressKey(int row, int col) {
  if (row == 0) {
    switch (col) {
      case 0: performClear();    return;
      case 1: performNegate();   return;
      case 2: performPercent();  return;
      case 3: inputOperation('/'); return;
    }
  }
  if (row >= 1 && row <= 3) {
    if (col >= 0 && col <= 2) {
      // digit: row1=7/8/9, row2=4/5/6, row3=1/2/3
      char digit = static_cast<char>('0' + (3 - row) * 3 + col + 1);
      inputDigit(digit);
      return;
    }
    if (col == 3) {
      // row1='*', row2='-', row3='+'
      char op;
      if (row == 1) op = '*';
      else if (row == 2) op = '-';
      else op = '+';
      inputOperation(op);
      return;
    }
  }
  if (row == 4) {
    switch (col) {
      case 0: inputDigit('0');   return;
      case 1: inputDecimal();    return;
      case 2: performBackspace(); return;
      case 3: performEquals();   return;
    }
  }
}

void CalculatorActivity::inputDigit(char digit) {
  if (newInput || showingResult) {
    strncpy(displayStr, "0", sizeof(displayStr));
    displayStr[1] = '\0';
    hasDecimal    = false;
    decimalPlaces = 0;
    newInput      = false;
    showingResult = false;
  }
  // Replace leading "0" unless it's before decimal
  int len = static_cast<int>(strlen(displayStr));
  if (len >= 15) return;  // cap at 15 chars
  if (len == 1 && displayStr[0] == '0' && !hasDecimal) {
    displayStr[0] = digit;
    displayStr[1] = '\0';
  } else {
    displayStr[len]     = digit;
    displayStr[len + 1] = '\0';
  }
  if (hasDecimal) decimalPlaces++;
  currentValue = atof(displayStr);
}

void CalculatorActivity::inputDecimal() {
  if (hasDecimal) return;
  if (newInput || showingResult) {
    strncpy(displayStr, "0", sizeof(displayStr));
    displayStr[1] = '\0';
    newInput      = false;
    showingResult = false;
    decimalPlaces = 0;
  }
  int len = static_cast<int>(strlen(displayStr));
  if (len >= 14) return;
  displayStr[len]     = '.';
  displayStr[len + 1] = '\0';
  hasDecimal    = true;
  decimalPlaces = 0;
  currentValue  = atof(displayStr);
}

void CalculatorActivity::inputOperation(char op) {
  if (pendingOp != '\0' && !newInput) {
    // Chain: evaluate previous operation
    double result = executeOp(accumulator, currentValue, pendingOp);
    accumulator = result;
    updateDisplayStr();
  } else {
    accumulator = currentValue;
  }
  pendingOp     = op;
  newInput      = true;
  showingResult = false;
  hasDecimal    = false;
  decimalPlaces = 0;

  // Build expression string: "12 +"
  char opDisplay = op;
  if (op == '*') opDisplay = 'x';
  snprintf(expressionStr, sizeof(expressionStr), "%.15g %c", accumulator, opDisplay);
}

void CalculatorActivity::performEquals() {
  if (pendingOp == '\0') return;
  double result = executeOp(accumulator, currentValue, pendingOp);

  // Build history expression: "12 + 5"
  char histExpr[40];
  char opDisplay = pendingOp;
  if (opDisplay == '*') opDisplay = 'x';
  snprintf(histExpr, sizeof(histExpr), "%.10g %c %.10g", accumulator, opDisplay, currentValue);

  addHistory(histExpr, result);

  accumulator   = result;
  currentValue  = result;
  pendingOp     = '\0';
  hasDecimal    = false;
  decimalPlaces = 0;
  newInput      = true;
  showingResult = true;
  expressionStr[0] = '\0';
  updateDisplayStr();
}

void CalculatorActivity::performClear() {
  accumulator    = 0.0;
  currentValue   = 0.0;
  pendingOp      = '\0';
  hasDecimal     = false;
  decimalPlaces  = 0;
  newInput       = true;
  showingResult  = false;
  strncpy(displayStr, "0", sizeof(displayStr));
  displayStr[1]    = '\0';
  expressionStr[0] = '\0';
}

void CalculatorActivity::performBackspace() {
  if (newInput || showingResult) return;
  int len = static_cast<int>(strlen(displayStr));
  if (len <= 1) {
    strncpy(displayStr, "0", sizeof(displayStr));
    displayStr[1] = '\0';
    currentValue  = 0.0;
    hasDecimal    = false;
    decimalPlaces = 0;
    return;
  }
  if (displayStr[len - 1] == '.') {
    hasDecimal    = false;
    decimalPlaces = 0;
  } else if (hasDecimal) {
    if (decimalPlaces > 0) decimalPlaces--;
  }
  displayStr[len - 1] = '\0';
  currentValue = atof(displayStr);
}

void CalculatorActivity::performPercent() {
  currentValue /= 100.0;
  updateDisplayStr();
}

void CalculatorActivity::performNegate() {
  currentValue = -currentValue;
  updateDisplayStr();
}

void CalculatorActivity::updateDisplayStr() {
  // Check if the value is an integer representable without fractional part
  double intPart;
  if (modf(currentValue, &intPart) == 0.0 &&
      currentValue >= -9999999999999.0 &&
      currentValue <=  9999999999999.0) {
    snprintf(displayStr, sizeof(displayStr), "%lld", static_cast<long long>(intPart));
  } else {
    snprintf(displayStr, sizeof(displayStr), "%.10g", currentValue);
  }
  // Safety truncate
  displayStr[15] = '\0';
  hasDecimal    = (strchr(displayStr, '.') != nullptr);
  decimalPlaces = 0;
  if (hasDecimal) {
    const char* dot = strchr(displayStr, '.');
    decimalPlaces = static_cast<int>(strlen(dot + 1));
  }
}

double CalculatorActivity::executeOp(double a, double b, char op) {
  switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/': return (b == 0.0) ? 0.0 : a / b;
    default:  return b;
  }
}

void CalculatorActivity::addHistory(const char* expr, double result) {
  if (historyCount < MAX_HISTORY) {
    strncpy(history[historyCount].expression, expr, 39);
    history[historyCount].expression[39] = '\0';
    history[historyCount].result = result;
    historyCount++;
  } else {
    // Shift entries down, drop oldest
    for (int i = 0; i < MAX_HISTORY - 1; i++) {
      history[i] = history[i + 1];
    }
    strncpy(history[MAX_HISTORY - 1].expression, expr, 39);
    history[MAX_HISTORY - 1].expression[39] = '\0';
    history[MAX_HISTORY - 1].result = result;
  }
}

// ----------------------------------------------------------------
// Rendering
// ----------------------------------------------------------------

// Layout constants (portrait 480x800)
static constexpr int HISTORY_TOP        = 5;
static constexpr int HISTORY_AREA_H     = 96;   // ~3 small-font rows @ 32px each
static constexpr int EXPRESSION_Y       = 104;
static constexpr int DISPLAY_TOP        = 122;
static constexpr int DISPLAY_H          = 64;
static constexpr int SEPARATOR_Y        = 194;
static constexpr int KEYPAD_TOP         = 206;
static constexpr int KEYPAD_BOTTOM      = 760;
static constexpr int KEYPAD_MARGIN_H    = 6;
static constexpr int KEYPAD_MARGIN_V    = 4;
static constexpr int KEY_GAP            = 3;

void CalculatorActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics  = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Calculator");

  drawHistory();
  drawDisplay();

  // Double separator line
  renderer.drawLine(8, SEPARATOR_Y,     pageWidth - 8, SEPARATOR_Y,     true);
  renderer.drawLine(8, SEPARATOR_Y + 2, pageWidth - 8, SEPARATOR_Y + 2, true);

  drawKeypad();

  const auto labels = mappedInput.mapLabels("Back", "Press", "Nav", "Nav");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void CalculatorActivity::drawHistory() {
  // Show last 3 history entries in SMALL_FONT
  const auto pageWidth = renderer.getScreenWidth();
  const int  show      = (historyCount < 3) ? historyCount : 3;
  const int  lineH     = 18;   // small font line height estimate
  const int  startIdx  = historyCount - show;

  for (int i = 0; i < show; i++) {
    const HistoryEntry& e    = history[startIdx + i];
    int                 y    = HISTORY_TOP + i * (lineH * 2 + 2);

    // Expression left-aligned
    renderer.drawText(SMALL_FONT_ID, 8, y, e.expression);

    // Result right-aligned on the next line
    char resBuf[24];
    double intPart;
    if (modf(e.result, &intPart) == 0.0 &&
        e.result >= -9999999999.0 && e.result <= 9999999999.0) {
      snprintf(resBuf, sizeof(resBuf), "= %lld", static_cast<long long>(intPart));
    } else {
      snprintf(resBuf, sizeof(resBuf), "= %.8g", e.result);
    }
    int tw = renderer.getTextWidth(SMALL_FONT_ID, resBuf);
    renderer.drawText(SMALL_FONT_ID, pageWidth - 8 - tw, y + lineH, resBuf);
  }
}

void CalculatorActivity::drawDisplay() {
  const auto pageWidth = renderer.getScreenWidth();

  // Light dither background for display area
  fillDithered25(renderer, 4, DISPLAY_TOP, pageWidth - 8, DISPLAY_H);

  // Border around display
  renderer.drawRect(4, DISPLAY_TOP, pageWidth - 8, DISPLAY_H, true);
  renderer.drawRect(5, DISPLAY_TOP + 1, pageWidth - 10, DISPLAY_H - 2, true);

  // Expression string (small, right-aligned, just above display box)
  if (expressionStr[0] != '\0') {
    int tw = renderer.getTextWidth(SMALL_FONT_ID, expressionStr);
    renderer.drawText(SMALL_FONT_ID, pageWidth - 8 - tw, EXPRESSION_Y, expressionStr);
  }

  // Main display number — large, right-aligned, bold
  int tw = renderer.getTextWidth(UI_12_FONT_ID, displayStr, EpdFontFamily::BOLD);
  int th = renderer.getTextHeight(UI_12_FONT_ID);
  int tx = pageWidth - 12 - tw;
  int ty = DISPLAY_TOP + (DISPLAY_H - th) / 2;

  // White fill under text so dither doesn't show through
  renderer.fillRect(tx - 2, ty, tw + 4, th, false);
  renderer.drawText(UI_12_FONT_ID, tx, ty, displayStr, true, EpdFontFamily::BOLD);
}

void CalculatorActivity::drawKeypad() {
  const auto pageWidth  = renderer.getScreenWidth();
  const int  availW     = pageWidth - 2 * KEYPAD_MARGIN_H;
  const int  availH     = KEYPAD_BOTTOM - KEYPAD_TOP - 2 * KEYPAD_MARGIN_V;
  const int  keyW       = (availW - (GRID_COLS - 1) * KEY_GAP) / GRID_COLS;
  const int  keyH       = (availH - (GRID_ROWS - 1) * KEY_GAP) / GRID_ROWS;
  const int  fontH      = renderer.getTextHeight(UI_10_FONT_ID);

  for (int row = 0; row < GRID_ROWS; row++) {
    for (int col = 0; col < GRID_COLS; col++) {
      const int kx = KEYPAD_MARGIN_H + col * (keyW + KEY_GAP);
      const int ky = KEYPAD_TOP + KEYPAD_MARGIN_V + row * (keyH + KEY_GAP);

      const bool selected = (row == cursorRow && col == cursorCol);
      // Operation keys: rightmost column (col==3) or top row ops (row==0, col>=1)
      const bool isOp     = (col == 3) || (row == 0 && col >= 1);

      if (selected) {
        // Solid black fill; label will be drawn in white (false = white pixel)
        renderer.fillRect(kx, ky, keyW, keyH, true);
      } else if (isOp) {
        // White background with dither texture
        renderer.fillRect(kx, ky, keyW, keyH, false);
        fillDithered25(renderer, kx + 1, ky + 1, keyW - 2, keyH - 2);
        renderer.drawRect(kx, ky, keyW, keyH, true);
      } else {
        // Plain white with border
        renderer.fillRect(kx, ky, keyW, keyH, false);
        renderer.drawRect(kx, ky, keyW, keyH, true);
      }

      // Center label text in the key
      const char* label = KEY_LABELS[row][col];
      int tw = renderer.getTextWidth(UI_10_FONT_ID, label);
      int tx = kx + (keyW - tw) / 2;
      int ty = ky + (keyH - fontH) / 2;

      // For selected keys draw white (black=false); otherwise black (black=true)
      renderer.drawText(UI_10_FONT_ID, tx, ty, label, !selected);
    }
  }
}
