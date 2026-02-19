#include "KeyboardEntryActivity.h"

#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/NetworkConstants.h"
#include "util/QRCodeHelper.h"

// Keyboard layouts - lowercase
const char* const KeyboardEntryActivity::keyboard[NUM_ROWS] = {
    "`1234567890-=", "qwertyuiop[]\\", "asdfghjkl;'", "zxcvbnm,./",
    "^  ____<QR OK"  // ^ = shift, _ = space, < = backspace, QR = remote input, OK = done
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

void KeyboardEntryActivity::onExit() {
  Activity::onExit();
  stopWebInputServer();
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
      return 12;  // shift(2), space(4), backspace(2), QR(2), OK(2)
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
  // Handle special row (bottom row with shift, space, backspace, QR, done)
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

    if (selectedCol >= BACKSPACE_COL && selectedCol < QR_COL) {
      // Backspace
      if (!text.empty()) {
        text.pop_back();
      }
      return;
    }

    if (selectedCol >= QR_COL && selectedCol < DONE_COL) {
      startWebInputServer();
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
  if (showingQR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      stopWebInputServer();
      showingQR = false;
      requestUpdate();
    }

    if (webInputServer && webInputServer->isRunning()) {
      webInputServer->handleClient();
      if (webInputServer->hasReceivedText()) {
        std::string received = webInputServer->consumeReceivedText();
        if (maxLength > 0) {
          const size_t remaining = text.length() < maxLength ? maxLength - text.length() : 0;
          if (remaining == 0) {
            received.clear();
          } else if (received.length() > remaining) {
            received.resize(remaining);
          }
        }
        text += received;
        stopWebInputServer();
        showingQR = false;
        requestUpdate();
      }
    }
    return;
  }

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
        // In shift key, wrap to end of row (OK)
        selectedCol = DONE_COL;
      } else if (selectedCol >= SPACE_COL && selectedCol < BACKSPACE_COL) {
        // In space bar, move to shift
        selectedCol = SHIFT_COL;
      } else if (selectedCol >= BACKSPACE_COL && selectedCol < QR_COL) {
        // In backspace, move to space
        selectedCol = SPACE_COL;
      } else if (selectedCol >= QR_COL && selectedCol < DONE_COL) {
        // In QR, move to backspace
        selectedCol = BACKSPACE_COL;
      } else if (selectedCol >= DONE_COL) {
        // At done button, move to QR
        selectedCol = QR_COL;
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
      } else if (selectedCol >= BACKSPACE_COL && selectedCol < QR_COL) {
        // In backspace, move to QR
        selectedCol = QR_COL;
      } else if (selectedCol >= QR_COL && selectedCol < DONE_COL) {
        // In QR, move to done
        selectedCol = DONE_COL;
      } else if (selectedCol >= DONE_COL) {
        // At done button, wrap to beginning of row (shift)
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
  if (showingQR) {
    renderQRScreen();
    return;
  }

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

  const char* const* layout = shiftState ? keyboardShift : keyboard;

  // Calculate left margin to center the longest row (13 keys)
  constexpr int maxRowWidth = KEYS_PER_ROW * (keyWidth + keySpacing);
  const int leftMargin = (pageWidth - maxRowWidth) / 2;

  for (int row = 0; row < NUM_ROWS; row++) {
    const int rowY = keyboardStartY + row * (keyHeight + keySpacing);

    // Left-align all rows for consistent navigation
    const int startX = leftMargin;

    // Handle bottom row (row 4) specially with proper multi-column keys
    if (row == 4) {
      // Bottom row layout: SHIFT (2 cols) | SPACE (4 cols) | <- (2 cols) | QR (2 cols) | OK (2 cols)

      int currentX = startX;

      // SHIFT key (logical col 0, spans 2 key widths)
      const bool shiftSelected = (selectedRow == 4 && selectedCol >= SHIFT_COL && selectedCol < SPACE_COL);
      static constexpr StrId shiftIds[3] = {StrId::STR_KBD_SHIFT, StrId::STR_KBD_SHIFT_CAPS, StrId::STR_KBD_LOCK};
      renderItemWithSelector(currentX + 2, rowY, I18N.get(shiftIds[shiftState]), shiftSelected);
      currentX += 2 * (keyWidth + keySpacing);

      // Space bar (logical cols 2-5, spans 4 key widths)
      const bool spaceSelected = (selectedRow == 4 && selectedCol >= SPACE_COL && selectedCol < BACKSPACE_COL);
      const int spaceTextWidth = renderer.getTextWidth(UI_10_FONT_ID, "____");
      const int spaceXWidth = 4 * (keyWidth + keySpacing);
      const int spaceXPos = currentX + (spaceXWidth - spaceTextWidth) / 2;
      renderItemWithSelector(spaceXPos, rowY, "____", spaceSelected);
      currentX += spaceXWidth;

      // Backspace key (logical cols 6-7, spans 2 key widths)
      const bool bsSelected = (selectedRow == 4 && selectedCol >= BACKSPACE_COL && selectedCol < QR_COL);
      renderItemWithSelector(currentX + 2, rowY, "<-", bsSelected);
      currentX += 2 * (keyWidth + keySpacing);

      // QR button (logical cols 8-9, spans 2 key widths)
      const bool qrSelected = (selectedRow == 4 && selectedCol >= QR_COL && selectedCol < DONE_COL);
      renderItemWithSelector(currentX + 2, rowY, "QR", qrSelected);
      currentX += 2 * (keyWidth + keySpacing);

      // OK button (logical cols 10-11, spans 2 key widths)
      const bool okSelected = (selectedRow == 4 && selectedCol >= DONE_COL);
      renderItemWithSelector(currentX + 2, rowY, tr(STR_OK_BUTTON), okSelected);
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
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Draw side button hints for Up/Down navigation
  GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));

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

void KeyboardEntryActivity::renderQRScreen() const {
  const auto pageWidth = renderer.getScreenWidth();

  constexpr int LINE_SPACING = 28;
  constexpr int QR_TOTAL = QRCodeHelper::qrSize();

  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Remote Text Input", true, EpdFontFamily::BOLD);

  if (webInputServer && webInputServer->isRunning()) {
    if (webInputServer->isApMode()) {
      // === AP mode layout (matching File Transfer) ===
      int apStartY = 55;

      renderer.drawCenteredText(UI_10_FONT_ID, apStartY, "Hotspot Mode", true, EpdFontFamily::BOLD);

      std::string ssidInfo = "Network: " + webInputServer->getApSSID();
      renderer.drawCenteredText(UI_10_FONT_ID, apStartY + LINE_SPACING, ssidInfo.c_str());

      renderer.drawCenteredText(SMALL_FONT_ID, apStartY + LINE_SPACING * 2, "Connect your device to this WiFi network");
      renderer.drawCenteredText(SMALL_FONT_ID, apStartY + LINE_SPACING * 3,
                                "or scan QR code with your phone to connect to Wifi.");

      const std::string wifiQR = webInputServer->getWifiQRString();
      QRCodeHelper::drawQRCode(renderer, (pageWidth - QR_TOTAL) / 2, apStartY + LINE_SPACING * 4, wifiQR);

      apStartY += QR_TOTAL - 4 * QRCodeHelper::DEFAULT_PX + 3 * LINE_SPACING;

      const std::string url = webInputServer->getUrl();
      renderer.drawCenteredText(UI_10_FONT_ID, apStartY + LINE_SPACING * 3, url.c_str(), true, EpdFontFamily::BOLD);

      std::string ipUrl = "or http://" + webInputServer->getIP() + "/";
      renderer.drawCenteredText(SMALL_FONT_ID, apStartY + LINE_SPACING * 4, ipUrl.c_str());
      renderer.drawCenteredText(SMALL_FONT_ID, apStartY + LINE_SPACING * 5, "Open this URL in your browser");
      renderer.drawCenteredText(SMALL_FONT_ID, apStartY + LINE_SPACING * 6, "or scan QR code with your phone:");
      QRCodeHelper::drawQRCode(renderer, (pageWidth - QR_TOTAL) / 2, apStartY + LINE_SPACING * 7, url);

    } else {
      // === STA mode layout (WiFi already connected, matching File Transfer) ===
      constexpr int staStartY = 65;

      const std::string ip = webInputServer->getIP();
      std::string ipInfo = "IP Address: " + ip;
      renderer.drawCenteredText(UI_10_FONT_ID, staStartY, ipInfo.c_str());

      std::string webUrl = "http://" + ip + "/";
      renderer.drawCenteredText(UI_10_FONT_ID, staStartY + LINE_SPACING * 2, webUrl.c_str(), true, EpdFontFamily::BOLD);

      std::string hostnameUrl = std::string("or http://") + NetworkConstants::AP_HOSTNAME + ".local/";
      renderer.drawCenteredText(SMALL_FONT_ID, staStartY + LINE_SPACING * 3, hostnameUrl.c_str());

      renderer.drawCenteredText(SMALL_FONT_ID, staStartY + LINE_SPACING * 4, "Open this URL in your browser");
      renderer.drawCenteredText(SMALL_FONT_ID, staStartY + LINE_SPACING * 5, "or scan QR code with your phone:");
      QRCodeHelper::drawQRCode(renderer, (pageWidth - QR_TOTAL) / 2, staStartY + LINE_SPACING * 6, webUrl);
    }
  } else {
    const auto pageHeight = renderer.getScreenHeight();
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 20, "Starting server...", true, EpdFontFamily::BOLD);
  }

  const auto labels = mappedInput.mapLabels("« Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void KeyboardEntryActivity::startWebInputServer() {
  if (!webInputServer) {
    webInputServer.reset(new KeyboardWebInputServer());
  }

  if (!webInputServer->isRunning()) {
    webInputServer->start();
  }

  showingQR = true;
  requestUpdate();
}

void KeyboardEntryActivity::stopWebInputServer() {
  if (webInputServer) {
    webInputServer->stop();
    webInputServer.reset();
  }
}
