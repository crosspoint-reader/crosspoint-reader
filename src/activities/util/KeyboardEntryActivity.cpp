#include "KeyboardEntryActivity.h"

#include "../../config.h"

// Keyboard layouts - lowercase
const char* const KeyboardEntryActivity::keyboard[NUM_ROWS] = {
    "`1234567890-=", "qwertyuiop[]\\", "asdfghjkl;'", "zxcvbnm,./",
    "^  _____<OK"  // ^ = shift, _ = space, < = backspace, OK = done
};

// Keyboard layouts - uppercase/symbols
const char* const KeyboardEntryActivity::keyboardShift[NUM_ROWS] = {"~!@#$%^&*()_+", "QWERTYUIOP{}|", "ASDFGHJKL:\"",
                                                                    "ZXCVBNM<>?", "^  _____<OK"};

KeyboardEntryActivity::KeyboardEntryActivity(GfxRenderer& renderer, InputManager& inputManager,
                                             const std::string& title, const std::string& initialText, size_t maxLength,
                                             bool isPassword)
    : Activity(renderer, inputManager), title(title), text(initialText), maxLength(maxLength), isPassword(isPassword) {}

void KeyboardEntryActivity::setText(const std::string& newText) {
  text = newText;
  if (maxLength > 0 && text.length() > maxLength) {
    text.resize(maxLength);
  }
}

void KeyboardEntryActivity::reset(const std::string& newTitle, const std::string& newInitialText) {
  if (!newTitle.empty()) {
    title = newTitle;
  }
  text = newInitialText;
  selectedRow = 0;
  selectedCol = 0;
  shiftActive = false;
  complete = false;
  cancelled = false;
}

void KeyboardEntryActivity::onEnter() {
  // Reset state when entering the activity
  complete = false;
  cancelled = false;
}

void KeyboardEntryActivity::onExit() {
  // Clean up if needed
}

void KeyboardEntryActivity::loop() {
  handleInput();
  render(10);
}

int KeyboardEntryActivity::getRowLength(int row) const {
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
      return 10;  // ^, space (5 wide), backspace, OK (2 wide)
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
  if (selectedRow == SHIFT_ROW) {
    if (selectedCol == SHIFT_COL) {
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

    if (selectedCol == BACKSPACE_COL) {
      // Backspace
      if (!text.empty()) {
        text.pop_back();
      }
      return;
    }

    if (selectedCol >= DONE_COL) {
      // Done button
      complete = true;
      if (onComplete) {
        onComplete(text);
      }
      return;
    }
  }

  // Regular character
  char c = getSelectedChar();
  if (c != '\0' && c != '^' && c != '_' && c != '<') {
    if (maxLength == 0 || text.length() < maxLength) {
      text += c;
      // Auto-disable shift after typing a letter
      if (shiftActive && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
        shiftActive = false;
      }
    }
  }
}

bool KeyboardEntryActivity::handleInput() {
  if (complete || cancelled) {
    return false;
  }

  bool handled = false;

  // Navigation
  if (inputManager.wasPressed(InputManager::BTN_UP)) {
    if (selectedRow > 0) {
      selectedRow--;
      // Clamp column to valid range for new row
      int maxCol = getRowLength(selectedRow) - 1;
      if (selectedCol > maxCol) selectedCol = maxCol;
    }
    handled = true;
  } else if (inputManager.wasPressed(InputManager::BTN_DOWN)) {
    if (selectedRow < NUM_ROWS - 1) {
      selectedRow++;
      int maxCol = getRowLength(selectedRow) - 1;
      if (selectedCol > maxCol) selectedCol = maxCol;
    }
    handled = true;
  } else if (inputManager.wasPressed(InputManager::BTN_LEFT)) {
    if (selectedCol > 0) {
      selectedCol--;
    } else if (selectedRow > 0) {
      // Wrap to previous row
      selectedRow--;
      selectedCol = getRowLength(selectedRow) - 1;
    }
    handled = true;
  } else if (inputManager.wasPressed(InputManager::BTN_RIGHT)) {
    int maxCol = getRowLength(selectedRow) - 1;
    if (selectedCol < maxCol) {
      selectedCol++;
    } else if (selectedRow < NUM_ROWS - 1) {
      // Wrap to next row
      selectedRow++;
      selectedCol = 0;
    }
    handled = true;
  }

  // Selection
  if (inputManager.wasPressed(InputManager::BTN_CONFIRM)) {
    handleKeyPress();
    handled = true;
  }

  // Cancel
  if (inputManager.wasPressed(InputManager::BTN_BACK)) {
    cancelled = true;
    if (onCancel) {
      onCancel();
    }
    handled = true;
  }

  return handled;
}

void KeyboardEntryActivity::render(int startY) const {
  const auto pageWidth = GfxRenderer::getScreenWidth();

  // Draw title
  renderer.drawCenteredText(UI_FONT_ID, startY, title.c_str(), true, REGULAR);

  // Draw input field
  int inputY = startY + 22;
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
  int charWidth = renderer.getSpaceWidth(UI_FONT_ID);
  if (charWidth < 1) charWidth = 8;  // Fallback to approximate width
  int maxDisplayLen = (pageWidth - 40) / charWidth;
  if (displayText.length() > static_cast<size_t>(maxDisplayLen)) {
    displayText = "..." + displayText.substr(displayText.length() - maxDisplayLen + 3);
  }

  renderer.drawText(UI_FONT_ID, 20, inputY, displayText.c_str());
  renderer.drawText(UI_FONT_ID, pageWidth - 15, inputY, "]");

  // Draw keyboard - use compact spacing to fit 5 rows on screen
  int keyboardStartY = inputY + 25;
  const int keyWidth = 18;
  const int keyHeight = 18;
  const int keySpacing = 3;

  const char* const* layout = shiftActive ? keyboardShift : keyboard;

  // Calculate left margin to center the longest row (13 keys)
  int maxRowWidth = KEYS_PER_ROW * (keyWidth + keySpacing);
  int leftMargin = (pageWidth - maxRowWidth) / 2;

  for (int row = 0; row < NUM_ROWS; row++) {
    int rowY = keyboardStartY + row * (keyHeight + keySpacing);

    // Left-align all rows for consistent navigation
    int startX = leftMargin;

    // Handle bottom row (row 4) specially with proper multi-column keys
    if (row == 4) {
      // Bottom row layout: CAPS (2 cols) | SPACE (5 cols) | <- (2 cols) | OK (2 cols)
      // Total: 11 visual columns, but we use logical positions for selection

      int currentX = startX;

      // CAPS key (logical col 0, spans 2 key widths)
      int capsWidth = 2 * keyWidth + keySpacing;
      bool capsSelected = (selectedRow == 4 && selectedCol == SHIFT_COL);
      if (capsSelected) {
        renderer.drawText(UI_FONT_ID, currentX - 2, rowY, "[");
        renderer.drawText(UI_FONT_ID, currentX + capsWidth - 4, rowY, "]");
      }
      renderer.drawText(UI_FONT_ID, currentX + 2, rowY, shiftActive ? "CAPS" : "caps");
      currentX += capsWidth + keySpacing;

      // Space bar (logical cols 2-6, spans 5 key widths)
      int spaceWidth = 5 * keyWidth + 4 * keySpacing;
      bool spaceSelected = (selectedRow == 4 && selectedCol >= SPACE_COL && selectedCol < BACKSPACE_COL);
      if (spaceSelected) {
        renderer.drawText(UI_FONT_ID, currentX - 2, rowY, "[");
        renderer.drawText(UI_FONT_ID, currentX + spaceWidth - 4, rowY, "]");
      }
      // Draw centered underscores for space bar
      int spaceTextX = currentX + (spaceWidth / 2) - 12;
      renderer.drawText(UI_FONT_ID, spaceTextX, rowY, "_____");
      currentX += spaceWidth + keySpacing;

      // Backspace key (logical col 7, spans 2 key widths)
      int bsWidth = 2 * keyWidth + keySpacing;
      bool bsSelected = (selectedRow == 4 && selectedCol == BACKSPACE_COL);
      if (bsSelected) {
        renderer.drawText(UI_FONT_ID, currentX - 2, rowY, "[");
        renderer.drawText(UI_FONT_ID, currentX + bsWidth - 4, rowY, "]");
      }
      renderer.drawText(UI_FONT_ID, currentX + 6, rowY, "<-");
      currentX += bsWidth + keySpacing;

      // OK button (logical col 9, spans 2 key widths)
      int okWidth = 2 * keyWidth + keySpacing;
      bool okSelected = (selectedRow == 4 && selectedCol >= DONE_COL);
      if (okSelected) {
        renderer.drawText(UI_FONT_ID, currentX - 2, rowY, "[");
        renderer.drawText(UI_FONT_ID, currentX + okWidth - 4, rowY, "]");
      }
      renderer.drawText(UI_FONT_ID, currentX + 8, rowY, "OK");

    } else {
      // Regular rows: render each key individually
      for (int col = 0; col < getRowLength(row); col++) {
        int keyX = startX + col * (keyWidth + keySpacing);

        // Get the character to display
        char c = layout[row][col];
        std::string keyLabel(1, c);

        // Draw selection highlight
        bool isSelected = (row == selectedRow && col == selectedCol);

        if (isSelected) {
          renderer.drawText(UI_FONT_ID, keyX - 2, rowY, "[");
          renderer.drawText(UI_FONT_ID, keyX + keyWidth - 4, rowY, "]");
        }

        renderer.drawText(UI_FONT_ID, keyX + 2, rowY, keyLabel.c_str());
      }
    }
  }

  // Draw help text at absolute bottom of screen (consistent with other screens)
  const auto pageHeight = GfxRenderer::getScreenHeight();
  renderer.drawText(SMALL_FONT_ID, 10, pageHeight - 30, "Navigate: D-pad | Select: OK | Cancel: BACK");
}
