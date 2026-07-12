#pragma once
#include <cstdint>
#include "activities/Activity.h"

class CalculatorActivity final : public Activity {
 public:
  explicit CalculatorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Calculator", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  double accumulator = 0;
  double currentValue = 0;
  char pendingOp = '\0';
  bool hasDecimal = false;
  int decimalPlaces = 0;
  bool newInput = true;
  bool showingResult = false;

  char displayStr[24] = "0";
  char expressionStr[40] = "";

  // 5 rows x 4 cols keypad grid
  int cursorRow = 3;
  int cursorCol = 0;
  static constexpr int GRID_ROWS = 5;
  static constexpr int GRID_COLS = 4;

  static const char* const KEY_LABELS[GRID_ROWS][GRID_COLS];

  struct HistoryEntry {
    char expression[40];
    double result;
  };
  static constexpr int MAX_HISTORY = 5;
  HistoryEntry history[MAX_HISTORY] = {};
  int historyCount = 0;

  void pressKey(int row, int col);
  void inputDigit(char digit);
  void inputDecimal();
  void inputOperation(char op);
  void performEquals();
  void performClear();
  void performBackspace();
  void performPercent();
  void performNegate();
  void updateDisplayStr();
  void addHistory(const char* expr, double result);
  double executeOp(double a, double b, char op);

  void drawDisplay();
  void drawKeypad();
  void drawHistory();

  static void fillDithered25(GfxRenderer& r, int x, int y, int w, int h);
};
