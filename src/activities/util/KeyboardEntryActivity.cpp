#include "KeyboardEntryActivity.h"

#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "core/features/FeatureModules.h"
#include "fontIds.h"
#include "network/RemoteKeyboardNetworkSession.h"
#include "network/RemoteKeyboardSession.h"
#include "util/QrUtils.h"

// Keyboard layouts - lowercase
const char* const KeyboardEntryActivity::keyboard[NUM_ROWS] = {
    "`1234567890-=", "qwertyuiop[]\\", "asdfghjkl;'", "zxcvbnm,./",
    "^  _____<OK"  // ^ = shift, _ = space, < = backspace, OK = done
};

// Keyboard layouts - uppercase/symbols
const char* const KeyboardEntryActivity::keyboardShift[NUM_ROWS] = {"~!@#$%^&*()_+", "QWERTYUIOP{}|", "ASDFGHJKL:\"",
                                                                    "ZXCVBNM<>?", "SPECIAL ROW"};

void KeyboardEntryActivity::onEnter() {
  Activity::onEnter();

  if (core::FeatureModules::hasCapability(core::Capability::RemoteKeyboardInput)) {
    inputMode = InputMode::Remote;
    remoteSessionId = REMOTE_KEYBOARD_SESSION.begin(title, text, maxLength, isPassword);
    remoteNetworkSession = std::make_unique<RemoteKeyboardNetworkSession>();
    remoteNetworkSession->begin();
    lastRemoteRefreshAt = 0;
  } else {
    inputMode = InputMode::Local;
  }

  // Trigger first update
  requestUpdate();
}

void KeyboardEntryActivity::onExit() {
  if (remoteSessionId != 0) {
    REMOTE_KEYBOARD_SESSION.cancel(remoteSessionId);
    remoteSessionId = 0;
  }
  if (remoteNetworkSession) {
    remoteNetworkSession->end();
    remoteNetworkSession.reset();
  }
  Activity::onExit();
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

bool KeyboardEntryActivity::handleKeyPress() {
  // Handle special row (bottom row with shift, space, backspace, done)
  if (selectedRow == SPECIAL_ROW) {
    if (selectedCol >= SHIFT_COL && selectedCol < SPACE_COL) {
      // Shift toggle
      shiftActive = !shiftActive;
      return true;
    }

    if (selectedCol >= SPACE_COL && selectedCol < BACKSPACE_COL) {
      // Space bar
      if (maxLength == 0 || text.length() < maxLength) {
        text += ' ';
      }
      return true;
    }

    if (selectedCol >= BACKSPACE_COL && selectedCol < DONE_COL) {
      // Backspace
      if (!text.empty()) {
        text.pop_back();
      }
      return true;
    }

    if (selectedCol >= DONE_COL) {
      // Done button
      onComplete(text);
      return false;
    }
  }

  // Regular character
  const char c = getSelectedChar();
  if (c == '\0') {
    return true;
  }

  if (maxLength == 0 || text.length() < maxLength) {
    text += c;
    // Auto-disable shift after typing a letter
    if (shiftActive && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
      shiftActive = false;
    }
  }

  return true;
}

void KeyboardEntryActivity::loop() {
  if (inputMode == InputMode::Remote) {
    if (remoteNetworkSession) {
      remoteNetworkSession->loop();
    }

    std::string submittedText;
    if (remoteSessionId != 0 && REMOTE_KEYBOARD_SESSION.takeSubmitted(remoteSessionId, submittedText)) {
      remoteSessionId = 0;
      if (remoteNetworkSession) {
        remoteNetworkSession->end();
        remoteNetworkSession.reset();
      }
      onComplete(std::move(submittedText));
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      switchToLocalInput();
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      if (remoteSessionId != 0) {
        REMOTE_KEYBOARD_SESSION.cancel(remoteSessionId);
        remoteSessionId = 0;
      }
      if (remoteNetworkSession) {
        remoteNetworkSession->end();
        remoteNetworkSession.reset();
      }
      onCancel();
      return;
    }

    if (millis() - lastRemoteRefreshAt >= 750) {
      lastRemoteRefreshAt = millis();
      requestUpdate();
    }
    return;
  }

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
    requestUpdate();
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
    requestUpdate();
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
      requestUpdate();
      return;
    }

    if (selectedCol > 0) {
      selectedCol--;
    } else {
      // Wrap to end of current row
      selectedCol = maxCol;
    }
    requestUpdate();
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
      requestUpdate();
      return;
    }

    if (selectedCol < maxCol) {
      selectedCol++;
    } else {
      // Wrap to beginning of current row
      selectedCol = 0;
    }
    requestUpdate();
  }

  // Selection
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (handleKeyPress()) {
      requestUpdate();
    }
    // If handleKeyPress returns false, it means onComplete was triggered, no update needed
  }

  // Cancel
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onCancel();
  }
}

void KeyboardEntryActivity::render(RenderLock&& lock) {
  if (inputMode == InputMode::Remote) {
    renderRemoteMode(std::move(lock));
    return;
  }

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();

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
      if (lineEndIdx == static_cast<int>(displayText.length())) {
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

bool KeyboardEntryActivity::skipLoopDelay() { return inputMode == InputMode::Remote; }

bool KeyboardEntryActivity::preventAutoSleep() { return inputMode == InputMode::Remote; }

bool KeyboardEntryActivity::blocksBackgroundServer() {
  return inputMode == InputMode::Remote && remoteNetworkSession && remoteNetworkSession->ownsServer();
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

void KeyboardEntryActivity::onComplete(std::string completedText) {
  setResult(KeyboardResult{std::move(completedText)});
  finish();
}

void KeyboardEntryActivity::onCancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void KeyboardEntryActivity::switchToLocalInput() {
  if (remoteSessionId != 0) {
    REMOTE_KEYBOARD_SESSION.cancel(remoteSessionId);
    remoteSessionId = 0;
  }
  if (remoteNetworkSession) {
    remoteNetworkSession->end();
    remoteNetworkSession.reset();
  }
  inputMode = InputMode::Local;
  requestUpdate();
}

void KeyboardEntryActivity::renderRemoteMode(RenderLock&&) {
  renderer.clearScreen();

  const auto snapshot = REMOTE_KEYBOARD_SESSION.snapshot();
  const auto network = remoteNetworkSession ? remoteNetworkSession->snapshot() : RemoteKeyboardNetworkSession::State{};
  const int pageWidth = renderer.getScreenWidth();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  renderer.drawCenteredText(UI_12_FONT_ID, 16, title.c_str(), true, EpdFontFamily::BOLD);

  std::string statusLine = "Type in the Android app or a browser";
  if (!snapshot.claimedBy.empty()) {
    statusLine = "Connected: " + snapshot.claimedBy;
  } else if (!network.ready) {
    statusLine = "Waiting for Android app or local fallback";
  } else if (network.apMode) {
    statusLine = "Hotspot ready for remote text input";
  } else {
    statusLine = "Scan the QR code or use the Android app";
  }
  renderer.drawCenteredText(UI_10_FONT_ID, 48, statusLine.c_str());

  std::string preview = snapshot.text;
  if (isPassword) {
    preview = std::string(preview.length(), '*');
  }
  if (preview.empty()) {
    preview = "(empty)";
  }
  if (preview.length() > 42) {
    preview.replace(39, preview.length() - 39, "...");
  }
  renderer.drawCenteredText(UI_10_FONT_ID, 48 + lineHeight + 8, preview.c_str());

  int y = 120;
  if (network.ready) {
    if (network.apMode) {
      renderer.drawCenteredText(UI_10_FONT_ID, y, "1. Join this hotspot", true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, y + lineHeight, network.ssid.c_str());
      QrUtils::drawQrCode(renderer, Rect{120, y + lineHeight + 18, 240, 180}, "WIFI:S:" + network.ssid + ";;");

      y += 230;
      renderer.drawCenteredText(UI_10_FONT_ID, y, "2. Open remote input", true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(SMALL_FONT_ID, y + lineHeight, network.url.c_str());
      QrUtils::drawQrCode(renderer, Rect{100, y + lineHeight + 18, 280, 220}, network.url);
      y += 280;
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, y, network.url.c_str(), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(SMALL_FONT_ID, y + lineHeight, "Scan to open the remote text field");
      QrUtils::drawQrCode(renderer, Rect{100, y + lineHeight + 24, 280, 280}, network.url);
      y += 330;
    }
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, y, "No network page available right now.");
    renderer.drawCenteredText(SMALL_FONT_ID, y + lineHeight + 8, "If the Android app is connected over USB or WiFi,");
    renderer.drawCenteredText(SMALL_FONT_ID, y + lineHeight * 2 + 8, "it can still pick up this input session.");
    y += 120;
  }

  renderer.drawCenteredText(SMALL_FONT_ID, y, "Press Select to use the on-device keyboard instead.");

  const auto labels = mappedInput.mapLabels("« Back", "Local", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
