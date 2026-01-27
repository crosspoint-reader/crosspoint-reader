#include "KeyboardEntryActivity.h"

#include "MappedInputManager.h"
#include "fontIds.h"

// Keyboard layouts - lowercase
const char* const KeyboardEntryActivity::keyboard[NUM_ROWS] = {
    "`1234567890-=", "qwertyuiop[]\\", "asdfghjkl;'", "zxcvbnm,./",
    "^  _____<OK"  // ^ = shift, _ = space, < = backspace, OK = done
};

// Keyboard layouts - uppercase/symbols
const char* const KeyboardEntryActivity::keyboardShift[NUM_ROWS] = {"~!@#$%^&*()_+", "QWERTYUIOP{}|", "ASDFGHJKL:\"",
                                                                    "ZXCVBNM<>?", "SPECIAL ROW"};

void KeyboardEntryActivity::taskTrampoline(void* param) {
  auto* self = static_cast<KeyboardEntryActivity*>(param);
  self->displayTaskLoop();
}

void KeyboardEntryActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void KeyboardEntryActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&KeyboardEntryActivity::taskTrampoline, "KeyboardEntryActivity",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void KeyboardEntryActivity::onExit() {
  Activity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

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
      return 10;  // shift (2 wide), space (5 wide), backspace (2 wide), OK
    default:
      return 0;
  }
}

char KeyboardEntryActivity::getSelectedChar() const {
  const char* const* layout = shiftActive ? keyboardShift : keyboard;

  if (selectedRow < 0 || selectedRow >= NUM_ROWS) return '\0';
  if (selectedCol < 0 || selectedCol >= getRowLength(selectedRow)) return '\0';

  return layout[selectedRow][selectedCol];
}

void KeyboardEntryActivity::handleKeyPress() {
  // Handle special row (bottom row with shift, space, backspace, done)
  if (selectedRow == SPECIAL_ROW) {
    if (selectedCol >= SHIFT_COL && selectedCol < SPACE_COL) {
      // Shift toggle
      shiftActive = !shiftActive;
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
    // Auto-disable shift after typing a letter
    if (shiftActive && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
      shiftActive = false;
    }
  }
}

void KeyboardEntryActivity::loop() {
  // Navigation
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    if (selectedRow > 0) {
      selectedRow--;
      // Clamp column to valid range for new row
      const int maxCol = getRowLength(selectedRow) - 1;
      if (selectedCol > maxCol) selectedCol = maxCol;
    } else {
      // Wrap to bottom row
      selectedRow = NUM_ROWS - 1;
      const int maxCol = getRowLength(selectedRow) - 1;
      if (selectedCol > maxCol) selectedCol = maxCol;
    }
    updateRequired = true;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    if (selectedRow < NUM_ROWS - 1) {
      selectedRow++;
      const int maxCol = getRowLength(selectedRow) - 1;
      if (selectedCol > maxCol) selectedCol = maxCol;
    } else {
      // Wrap to top row
      selectedRow = 0;
      const int maxCol = getRowLength(selectedRow) - 1;
      if (selectedCol > maxCol) selectedCol = maxCol;
    }
    updateRequired = true;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
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
      updateRequired = true;
      return;
    }

    if (selectedCol > 0) {
      selectedCol--;
    } else {
      // Wrap to end of current row
      selectedCol = maxCol;
    }
    updateRequired = true;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
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
      updateRequired = true;
      return;
    }

    if (selectedCol < maxCol) {
      selectedCol++;
    } else {
      // Wrap to beginning of current row
      selectedCol = 0;
    }
    updateRequired = true;
  }

  // Selection
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleKeyPress();
    updateRequired = true;
  }

  // Cancel
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (onCancel) {
      onCancel();
    }
    updateRequired = true;
  }
}

void KeyboardEntryActivity::render() const {
  const auto pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();

  // Draw title
  renderer.drawCenteredText(UI_10_FONT_ID, startY, title.c_str());

  // Draw input field
  const int inputStartY = startY + 22;
  int inputEndY = startY + 22;
  renderer.drawText(UI_10_FONT_ID, 10, inputStartY, "[");

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
  while (true) {
    std::string lineText = displayText.substr(lineStartIdx, lineEndIdx - lineStartIdx);
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, lineText.c_str());
    if (textWidth <= pageWidth - 40) {
      renderer.drawText(UI_10_FONT_ID, 20, inputEndY, lineText.c_str());
      if (lineEndIdx == displayText.length()) {
        break;
      }

      inputEndY += renderer.getLineHeight(UI_10_FONT_ID);
      lineStartIdx = lineEndIdx;
      lineEndIdx = displayText.length();
    } else {
      lineEndIdx -= 1;
    }
  }
  renderer.drawText(UI_10_FONT_ID, pageWidth - 15, inputEndY, "]");

  // Draw keyboard - use compact spacing to fit 5 rows on screen
  const int keyboardStartY = inputEndY + 25;
  constexpr int keyWidth = 18;
  constexpr int keyHeight = 18;
  constexpr int keySpacing = 3;

  const char* const* layout = shiftActive ? keyboardShift : keyboard;

  // Calculate left margin to center the longest row (13 keys)
  constexpr int maxRowWidth = KEYS_PER_ROW * (keyWidth + keySpacing);
  const int leftMargin = (pageWidth - maxRowWidth) / 2;

  for (int row = 0; row < NUM_ROWS; row++) {
    const int rowY = keyboardStartY + row * (keyHeight + keySpacing);

    // Left-align all rows for consistent navigation
    const int startX = leftMargin;

    // Handle bottom row (row 4) specially with proper multi-column keys
    if (row == 4) {
      // Bottom row layout: SHIFT (2 cols) | SPACE (5 cols) | <- (2 cols) | OK (2 cols)
      // Total: 11 visual columns, but we use logical positions for selection

      int currentX = startX;

      // SHIFT key (logical col 0, spans 2 key widths)
      const bool shiftSelected = (selectedRow == 4 && selectedCol >= SHIFT_COL && selectedCol < SPACE_COL);
      renderItemWithSelector(currentX + 2, rowY, shiftActive ? "SHIFT" : "shift", shiftSelected);
      currentX += 2 * (keyWidth + keySpacing);

      // Space bar (logical cols 2-6, spans 5 key widths)
      const bool spaceSelected = (selectedRow == 4 && selectedCol >= SPACE_COL && selectedCol < BACKSPACE_COL);
      const int spaceTextWidth = renderer.getTextWidth(UI_10_FONT_ID, "_____");
      const int spaceXWidth = 5 * (keyWidth + keySpacing);
      const int spaceXPos = currentX + (spaceXWidth - spaceTextWidth) / 2;
      renderItemWithSelector(spaceXPos, rowY, "_____", spaceSelected);
      currentX += spaceXWidth;

      // Backspace key (logical col 7, spans 2 key widths)
      const bool bsSelected = (selectedRow == 4 && selectedCol >= BACKSPACE_COL && selectedCol < DONE_COL);
      renderItemWithSelector(currentX + 2, rowY, "<-", bsSelected);
      currentX += 2 * (keyWidth + keySpacing);

      // OK button (logical col 9, spans 2 key widths)
      const bool okSelected = (selectedRow == 4 && selectedCol >= DONE_COL);
      renderItemWithSelector(currentX + 2, rowY, "OK", okSelected);
    } else {
      // Regular rows: render each key individually
      for (int col = 0; col < getRowLength(row); col++) {
        // Get the character to display
        const char c = layout[row][col];
        std::string keyLabel(1, c);
        const int charWidth = renderer.getTextWidth(UI_10_FONT_ID, keyLabel.c_str());

        const int keyX = startX + col * (keyWidth + keySpacing) + (keyWidth - charWidth) / 2;
        const bool isSelected = row == selectedRow && col == selectedCol;
        renderItemWithSelector(keyX, rowY, keyLabel.c_str(), isSelected);
      }
    }
  }

  // Draw help text
  const auto labels = mappedInput.mapLabels("« Back", "Select", "Left", "Right");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Draw side button hints for Up/Down navigation
  renderer.drawSideButtonHints(UI_10_FONT_ID, "Up", "Down");

  renderer.displayBuffer();
}

void KeyboardEntryActivity::renderItemWithSelector(const int x, const int y, const char* item,
                                                   const bool isSelected) const {
  if (isSelected) {
    const int itemWidth = renderer.getTextWidth(UI_10_FONT_ID, item);
    renderer.drawText(UI_10_FONT_ID, x - 6, y, "[");
    renderer.drawText(UI_10_FONT_ID, x + itemWidth, y, "]");
  }
  renderer.drawText(UI_10_FONT_ID, x, y, item);
}
