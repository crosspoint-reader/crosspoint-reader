#pragma once
#include <GfxRenderer.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

struct KeyDef {
  char primary;
  char secondary;
};

enum SpecialKeyType { SpecShift, SpecMode, SpecSpace, SpecDel, SpecOk };

class KeyboardEntryActivity : public Activity {
 public:
  explicit KeyboardEntryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 std::string title = "Enter Text", std::string initialText = "",
                                 const size_t maxLength = 0, const bool isPassword = false)
      : Activity("KeyboardEntry", renderer, mappedInput),
        title(std::move(title)),
        text(std::move(initialText)),
        maxLength(maxLength),
        isPassword(isPassword) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string title;
  std::string text;
  size_t maxLength;
  bool isPassword;

  ButtonNavigator buttonNavigator;

  int selectedRow = 0;
  int selectedCol = 0;
  int shiftState = 0;
  bool symMode = false;
  bool confirmHeld = false;
  bool confirmLongHandled = false;

  void onComplete(std::string text);
  void onCancel();

  static constexpr uint16_t LONG_PRESS_MS = 500;

  static constexpr int COLS = 10;
  static constexpr int ABC_ROWS = 4;
  static constexpr int SYM_ROWS = 4;
  static constexpr int BOTTOM_KEY_COUNT = 5;

  static constexpr KeyDef abcLayout[ABC_ROWS][COLS] = {
      {{'0', ')'},
       {'1', '!'},
       {'2', '@'},
       {'3', '#'},
       {'4', '$'},
       {'5', '%'},
       {'6', '^'},
       {'7', '&'},
       {'8', '*'},
       {'9', '('}},
      {{'q', 'Q'},
       {'w', 'W'},
       {'e', 'E'},
       {'r', 'R'},
       {'t', 'T'},
       {'y', 'Y'},
       {'u', 'U'},
       {'i', 'I'},
       {'o', 'O'},
       {'p', 'P'}},
      {{'a', 'A'},
       {'s', 'S'},
       {'d', 'D'},
       {'f', 'F'},
       {'g', 'G'},
       {'h', 'H'},
       {'j', 'J'},
       {'k', 'K'},
       {'l', 'L'},
       {'-', '_'}},
      {{'z', 'Z'},
       {'x', 'X'},
       {'c', 'C'},
       {'v', 'V'},
       {'b', 'B'},
       {'n', 'N'},
       {'m', 'M'},
       {'=', '+'},
       {'.', '>'},
       {',', '<'}},
  };

  static constexpr KeyDef symLayout[SYM_ROWS][COLS] = {
      {{'0', '\0'},
       {'1', '\0'},
       {'2', '\0'},
       {'3', '\0'},
       {'4', '\0'},
       {'5', '\0'},
       {'6', '\0'},
       {'7', '\0'},
       {'8', '\0'},
       {'9', '\0'}},
      {{')', '\0'},
       {'!', '\0'},
       {'@', '\0'},
       {'#', '\0'},
       {'$', '\0'},
       {'%', '\0'},
       {'^', '\0'},
       {'&', '\0'},
       {'*', '\0'},
       {'(', '\0'}},
      {{'-', '\0'},
       {'_', '\0'},
       {'=', '\0'},
       {'+', '\0'},
       {'[', '\0'},
       {']', '\0'},
       {'{', '\0'},
       {'}', '\0'},
       {';', '\0'},
       {':', '\0'}},
      {{'\'', '\0'},
       {'"', '\0'},
       {'/', '\0'},
       {'\\', '\0'},
       {'|', '\0'},
       {'?', '\0'},
       {'.', '\0'},
       {',', '\0'},
       {'~', '\0'},
       {'`', '\0'}},
  };

  static const char* const shiftString[2];

  int getContentRowCount() const;
  int getTotalRowCount() const;
  bool isBottomRow(int row) const;
  char getSelectedChar() const;
  char getAlternativeChar() const;
  bool handleKeyPress();
  bool insertChar(char c);
};
