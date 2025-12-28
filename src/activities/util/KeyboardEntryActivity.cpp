#include "KeyboardEntryActivity.h"

#include "../../config.h"

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
      return 10;  // caps (2 wide), space (5 wide), backspace (2 wide), OK
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
  if (inputManager.wasPressed(InputManager::BTN_UP)) {
    if (selectedRow > 0) {
      selectedRow--;
      // Clamp column to valid range for new row
      const int maxCol = getRowLength(selectedRow) - 1;
      if (selectedCol > maxCol) selectedCol = maxCol;
    }
    updateRequired = true;
  }

  if (inputManager.wasPressed(InputManager::BTN_DOWN)) {
    if (selectedRow < NUM_ROWS - 1) {
      selectedRow++;
      const int maxCol = getRowLength(selectedRow) - 1;
      if (selectedCol > maxCol) selectedCol = maxCol;
    }
    updateRequired = true;
  }

  if (inputManager.wasPressed(InputManager::BTN_LEFT)) {
    // Special bottom row case
    if (selectedRow == SPECIAL_ROW) {
      // Bottom row has special key widths
      if (selectedCol >= SHIFT_COL && selectedCol < SPACE_COL) {
        // In shift key, do nothing
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
    } else if (selectedRow > 0) {
      // Wrap to previous row
      selectedRow--;
      selectedCol = getRowLength(selectedRow) - 1;
    }
    updateRequired = true;
  }

  if (inputManager.wasPressed(InputManager::BTN_RIGHT)) {
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
        // At done button, do nothing
      }
      updateRequired = true;
      return;
    }

    if (selectedCol < maxCol) {
      selectedCol++;
    } else if (selectedRow < NUM_ROWS - 1) {
      // Wrap to next row
      selectedRow++;
      selectedCol = 0;
    }
    updateRequired = true;
  }

  // Selection
  if (inputManager.wasPressed(InputManager::BTN_CONFIRM)) {
    handleKeyPress();
    updateRequired = true;
  }

  // Cancel
  if (inputManager.wasPressed(InputManager::BTN_BACK)) {
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
  renderer.drawCenteredText(UI_FONT_ID, startY, title.c_str(), true, REGULAR);

  // Draw input field
  const int inputY = startY + 22;
  renderer.drawText(UI_FONT_ID, 10, inputY, "[");

  std::string displayText;
  if (isPassword) {
    displayText = std::string(text.length(), '*');
  } else {
    displayText = text;
  }

  // Show cursor at end
  displayText += "_";

  // Truncate if too long for display - use actual character width from font
  int approxCharWidth = renderer.getSpaceWidth(UI_FONT_ID);
  if (approxCharWidth < 1) approxCharWidth = 8;  // Fallback to approximate width
  const int maxDisplayLen = (pageWidth - 40) / approxCharWidth;
  if (displayText.length() > static_cast<size_t>(maxDisplayLen)) {
    displayText = "..." + displayText.substr(displayText.length() - maxDisplayLen + 3);
  }

  renderer.drawText(UI_FONT_ID, 20, inputY, displayText.c_str());
  renderer.drawText(UI_FONT_ID, pageWidth - 15, inputY, "]");

  // Draw keyboard - use compact spacing to fit 5 rows on screen
  const int keyboardStartY = inputY + 25;
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
      // Bottom row layout: CAPS (2 cols) | SPACE (5 cols) | <- (2 cols) | OK (2 cols)
      // Total: 11 visual columns, but we use logical positions for selection

      int currentX = startX;

      // CAPS key (logical col 0, spans 2 key widths)
      const bool capsSelected = (selectedRow == 4 && selectedCol >= SHIFT_COL && selectedCol < SPACE_COL);
      renderItemWithSelector(currentX + 2, rowY, shiftActive ? "CAPS" : "caps", capsSelected);
      currentX += 2 * (keyWidth + keySpacing);

      // Space bar (logical cols 2-6, spans 5 key widths)
      const bool spaceSelected = (selectedRow == 4 && selectedCol >= SPACE_COL && selectedCol < BACKSPACE_COL);
      const int spaceTextWidth = renderer.getTextWidth(UI_FONT_ID, "_____");
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
        const int charWidth = renderer.getTextWidth(UI_FONT_ID, keyLabel.c_str());

        const int keyX = startX + col * (keyWidth + keySpacing) + (keyWidth - charWidth) / 2;
        const bool isSelected = row == selectedRow && col == selectedCol;
        renderItemWithSelector(keyX, rowY, keyLabel.c_str(), isSelected);
      }
    }
  }

  // Draw help text at absolute bottom of screen (consistent with other screens)
  const auto pageHeight = renderer.getScreenHeight();
  renderer.drawText(SMALL_FONT_ID, 10, pageHeight - 30, "Navigate: D-pad | Select: OK | Cancel: BACK");
  renderer.displayBuffer();
}

void KeyboardEntryActivity::renderItemWithSelector(const int x, const int y, const char* item,
                                                   const bool isSelected) const {
  if (isSelected) {
    const int itemWidth = renderer.getTextWidth(UI_FONT_ID, item);
    renderer.drawText(UI_FONT_ID, x - 6, y, "[");
    renderer.drawText(UI_FONT_ID, x + itemWidth, y, "]");
  }
  renderer.drawText(UI_FONT_ID, x, y, item);
}
