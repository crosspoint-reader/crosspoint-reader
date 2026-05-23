#pragma once

#include "MappedInputManager.h"
#include "activities/Activity.h"

// Numeric input dialog for "jump to printed page".
// User adjusts a single integer (mirroring the printed-page label as shown in the book).
// Up/Down change the digit under the cursor by ±1 (single) or ±10 (double-click).
// Left/Right move the cursor between digits; Confirm returns the typed string.
// Books with non-integer labels (roman numerals, etc.) are not addressable via this dialog;
// the menu item is hidden if the book has no integer-parseable printed-page labels.
class EpubReaderPrintedPageInputActivity final : public Activity {
 public:
  explicit EpubReaderPrintedPageInputActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int initialValue,
                                              int minValue, int maxValue)
      : Activity("EpubReaderPrintedPageInput", renderer, mappedInput),
        value(initialValue),
        minValue(minValue),
        maxValue(maxValue) {
    clampValue();
    cursorDigit = 0;  // ones place
  }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  int value = 0;
  int minValue = 1;
  int maxValue = 1;
  int cursorDigit = 0;  // 0 = ones, 1 = tens, 2 = hundreds, ...

  void clampValue();
  void adjustDigit(int delta);
  void adjustDigitTimes(int multiplier, int sign);  // delta = sign * multiplier * 10^cursorDigit
  void moveCursor(int delta);
  static int powTen(int exponent);
  int digitCount() const;
  // Highest cursor position the user can reach: one less than the digit count of maxValue.
  // Lets the user move the cursor "past" the current value to higher digit positions that
  // don't exist yet, so they can grow the number quickly (e.g. start at 1, move cursor left,
  // press Up to make 11, etc.) without single-pressing dozens of times.
  int maxCursorDigit() const;
};
