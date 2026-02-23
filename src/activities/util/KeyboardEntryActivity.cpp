#include "KeyboardEntryActivity.h"

#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

// Keyboard layouts - lowercase
const char* const KeyboardEntryActivity::keyboard[NUM_ROWS] = {
    "`1234567890-=", "qwertyuiop[]\\", "asdfghjkl;'", "zxcvbnm,./",
    "^  _____<OK"  // ^ = shift, _ = space, < = backspace, OK = done
};

// Keyboard layouts - uppercase/symbols
const char* const KeyboardEntryActivity::keyboardShift[NUM_ROWS] = {"~!@#$%^&*()_+", "QWERTYUIOP{}|", "ASDFGHJKL:\"",
                                                                    "ZXCVBNM<>?", "SPECIAL ROW"};

// Shift state strings
const char* const KeyboardEntryActivity::shiftString[3] = {"shift", "SHIFT", "LOCK"};

void KeyboardEntryActivity::onEnter() {
  Activity::onEnter();

  // Trigger first update
  requestUpdate();
}

void KeyboardEntryActivity::onExit() { Activity::onExit(); }

int KeyboardEntryActivity::getRowLength(const int row) const {
  if (row < 0 || row >= NUM_ROWS) return 0;

  // Return actual length of each row based on keyboard layout
  switch (row) {
    case 0:
      return 13;  // `1234567890-=
    case 1:
      return 13;  // qwertyuiop[]backslash
    case 2:
      return 11;  // asdfghjkl;'
    case 3:
      return 10;  // zxcvbnm,./
    case 4:
      return 11;  // shift (2 wide), space (5 wide), backspace (2 wide), OK (2 wide)
    default:
      return 0;
  }
}

char KeyboardEntryActivity::getSelectedChar() const {
  const char* const* layout = shiftState ? keyboardShift : keyboard;

  if (selectedRow < 0 || selectedRow >= NUM_ROWS) return '\0';
  if (selectedCol < 0 || selectedCol >= getRowLength(selectedRow)) return '\0';

  return layout[selectedRow][selectedCol];
}

void KeyboardEntryActivity::handleKeyPress() {
  // Handle special row (bottom row with shift, space, backspace, done)
  if (selectedRow == SPECIAL_ROW) {
    if (selectedCol >= SHIFT_COL && selectedCol < SPACE_COL) {
      // Shift toggle (0 = lower case, 1 = upper case, 2 = shift lock)
      shiftState = (shiftState + 1) % 3;
      return;
    }

    if (selectedCol >= SPACE_COL && selectedCol < BACKSPACE_COL) {
      // Space bar
      if (maxLength == 0 || text.length() < maxLength) {
        text += ' ';
      }
      return;
    }

    if (selectedCol >= BACKSPACE_COL && selectedCol < DONE_COL) {
      // Backspace
      if (!text.empty()) {
        text.pop_back();
      }
      return;
    }

    if (selectedCol >= DONE_COL) {
      // Done button
      if (onComplete) {
        onComplete(text);
      }
      return;
    }
  }

  // Regular character
  const char c = getSelectedChar();
  if (c == '\0') {
    return;
  }

  if (maxLength == 0 || text.length() < maxLength) {
    text += c;
    // Auto-disable shift after typing a character in non-lock mode
    if (shiftState == 1) {
      shiftState = 0;
    }
  }
}

void KeyboardEntryActivity::loop() {
  // Handle navigation
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] {
    selectedRow = ButtonNavigator::previousIndex(selectedRow, NUM_ROWS);

    const int maxCol = getRowLength(selectedRow) - 1;
    if (selectedCol > maxCol) selectedCol = maxCol;
    requestUpdate();
  });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] {
    selectedRow = ButtonNavigator::nextIndex(selectedRow, NUM_ROWS);

    const int maxCol = getRowLength(selectedRow) - 1;
    if (selectedCol > maxCol) selectedCol = maxCol;
    requestUpdate();
  });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] {
    const int maxCol = getRowLength(selectedRow) - 1;

    // Special bottom row case
    if (selectedRow == SPECIAL_ROW) {
      // Bottom row has special key widths
      if (selectedCol >= SHIFT_COL && selectedCol < SPACE_COL) {
        // In shift key, wrap to end of row
        selectedCol = maxCol;
      } else if (selectedCol >= SPACE_COL && selectedCol < BACKSPACE_COL) {
        // In space bar, move to shift
        selectedCol = SHIFT_COL;
      } else if (selectedCol >= BACKSPACE_COL && selectedCol < DONE_COL) {
        // In backspace, move to space
        selectedCol = SPACE_COL;
      } else if (selectedCol >= DONE_COL) {
        // At done button, move to backspace
        selectedCol = BACKSPACE_COL;
      }
    } else {
      selectedCol = ButtonNavigator::previousIndex(selectedCol, maxCol + 1);
    }

    requestUpdate();
  });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] {
    const int maxCol = getRowLength(selectedRow) - 1;

    // Special bottom row case
    if (selectedRow == SPECIAL_ROW) {
      // Bottom row has special key widths
      if (selectedCol >= SHIFT_COL && selectedCol < SPACE_COL) {
        // In shift key, move to space
        selectedCol = SPACE_COL;
      } else if (selectedCol >= SPACE_COL && selectedCol < BACKSPACE_COL) {
        // In space bar, move to backspace
        selectedCol = BACKSPACE_COL;
      } else if (selectedCol >= BACKSPACE_COL && selectedCol < DONE_COL) {
        // In backspace, move to done
        selectedCol = DONE_COL;
      } else if (selectedCol >= DONE_COL) {
        // At done button, wrap to beginning of row
        selectedCol = SHIFT_COL;
      }
    } else {
      selectedCol = ButtonNavigator::nextIndex(selectedCol, maxCol + 1);
    }
    requestUpdate();
  });

  // Selection
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleKeyPress();
    requestUpdate();
  }

  // Cancel
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (onCancel) {
      onCancel();
    }
    requestUpdate();
  }
}

void KeyboardEntryActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title.c_str());

  // Draw input field
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int inputStartY =
      metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + metrics.verticalSpacing * 4;
  int inputHeight = 0;

  std::string displayText;
  if (isPassword) {
    displayText = std::string(text.length(), '*');
  } else {
    displayText = text;
  }

  // Show cursor at end
  displayText += "_";

  // Render input text across multiple lines
  int lineStartIdx = 0;
  int lineEndIdx = displayText.length();
  int textWidth = 0;
  while (true) {
    std::string lineText = displayText.substr(lineStartIdx, lineEndIdx - lineStartIdx);
    textWidth = renderer.getTextWidth(UI_12_FONT_ID, lineText.c_str());
    if (textWidth <= pageWidth - 2 * metrics.contentSidePadding) {
      if (metrics.keyboardCenteredText) {
        renderer.drawCenteredText(UI_12_FONT_ID, inputStartY + inputHeight, lineText.c_str());
      } else {
        renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, inputStartY + inputHeight, lineText.c_str());
      }
      if (lineEndIdx == displayText.length()) {
        break;
      }

      inputHeight += lineHeight;
      lineStartIdx = lineEndIdx;
      lineEndIdx = displayText.length();
    } else {
      lineEndIdx -= 1;
    }
  }

  GUI.drawTextField(renderer, Rect{0, inputStartY, pageWidth, inputHeight}, textWidth);

  // Draw keyboard - use compact spacing to fit 5 rows on screen
  const int keyboardStartY = metrics.keyboardBottomAligned
                                 ? pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing -
                                       (metrics.keyboardKeyHeight + metrics.keyboardKeySpacing) * NUM_ROWS
                                 : inputStartY + inputHeight + metrics.verticalSpacing * 4;
  const int keyWidth = metrics.keyboardKeyWidth;
  const int keyHeight = metrics.keyboardKeyHeight;
  const int keySpacing = metrics.keyboardKeySpacing;

  const char* const* layout = shiftState ? keyboardShift : keyboard;

  // Calculate left margin to center the longest row (13 keys)
  const int maxRowWidth = KEYS_PER_ROW * (keyWidth + keySpacing);
  const int leftMargin = (pageWidth - maxRowWidth) / 2;

  for (int row = 0; row < NUM_ROWS; row++) {
    const int rowY = keyboardStartY + row * (keyHeight + keySpacing);

    // Left-align all rows for consistent navigation
    const int startX = leftMargin;

    // Handle bottom row (row 4) specially with proper multi-column keys
    if (row == SPECIAL_ROW) {
      // Bottom row layout: SHIFT (2 cols) | SPACE (5 cols) | <- (2 cols) | OK (2 cols)
      // Total: 11 visual columns, but we use logical positions for selection

      int currentX = startX;

      // SHIFT key (logical col 0, spans 2 key widths)
      const bool shiftSelected = (selectedRow == SPECIAL_ROW && selectedCol >= SHIFT_COL && selectedCol < SPACE_COL);
      const int shiftWidth = SPACE_COL - SHIFT_COL;
      const int shiftXWidth = shiftWidth * (keyWidth + keySpacing);
      GUI.drawKeyboardKey(renderer, Rect{currentX, rowY, shiftXWidth, keyHeight}, shiftString[shiftState],
                          shiftSelected);
      currentX += shiftXWidth;

      // Space bar (logical cols 2-6, spans 5 key widths)
      const bool spaceSelected =
          (selectedRow == SPECIAL_ROW && selectedCol >= SPACE_COL && selectedCol < BACKSPACE_COL);
      const int spaceWidth = BACKSPACE_COL - SPACE_COL;
      const int spaceXWidth = spaceWidth * (keyWidth + keySpacing);
      GUI.drawKeyboardKey(renderer, Rect{currentX, rowY, spaceXWidth, keyHeight}, "_____", spaceSelected);
      currentX += spaceXWidth;

      // Backspace key (logical col 7, spans 2 key widths)
      const bool bsSelected = (selectedRow == SPECIAL_ROW && selectedCol >= BACKSPACE_COL && selectedCol < DONE_COL);
      const int backspaceWidth = DONE_COL - BACKSPACE_COL;
      const int backspaceXWidth = backspaceWidth * (keyWidth + keySpacing);
      GUI.drawKeyboardKey(renderer, Rect{currentX, rowY, backspaceXWidth, keyHeight}, "<-", bsSelected);
      currentX += backspaceXWidth;

      // OK button (logical col 9, spans 2 key widths)
      const bool okSelected = (selectedRow == SPECIAL_ROW && selectedCol >= DONE_COL);
      const int okWidth = getRowLength(row) - DONE_COL;
      const int okXWidth = okWidth * (keyWidth + keySpacing);
      GUI.drawKeyboardKey(renderer, Rect{currentX, rowY, okXWidth, keyHeight}, tr(STR_OK_BUTTON), okSelected);
    } else {
      // Regular rows: render each key individually
      for (int col = 0; col < getRowLength(row); col++) {
        // Get the character to display
        const char c = layout[row][col];
        std::string keyLabel(1, c);

        const int keyX = startX + col * (keyWidth + keySpacing);
        const bool isSelected = row == selectedRow && col == selectedCol;
        GUI.drawKeyboardKey(renderer, Rect{keyX, rowY, keyWidth, keyHeight}, keyLabel.c_str(), isSelected);
      }
    }
  }

  // Draw help text
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Draw side button hints for Up/Down navigation
  GUI.drawSideButtonHints(renderer, ">", "<");

  renderer.displayBuffer();
}
