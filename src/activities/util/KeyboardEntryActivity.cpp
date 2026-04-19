#include "KeyboardEntryActivity.h"

#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

const char* const KeyboardEntryActivity::shiftString[2] = {"shift", "SHIFT"};

void KeyboardEntryActivity::onEnter() {
  Activity::onEnter();
  cursorPos = text.length();
  symMode = false;
  urlMode = false;
  cursorMode = false;
  togglePos = false;
  passwordVisible = false;
  shiftState = 0;
  selectedRow = 0;
  selectedCol = 0;
  delPressCount = 0;
  hintVisible = false;
  hintShowTime = 0;
  rightHeld = false;
  rightLongHandled = false;
  savedCursorPos = 0;
  rightStartCursorPos = 0;
  requestUpdate();
}

void KeyboardEntryActivity::onExit() { Activity::onExit(); }

int KeyboardEntryActivity::getContentRowCount() const {
  if (urlMode) return 3;
  return ABC_ROWS;
}

int KeyboardEntryActivity::getContentColCount() const {
  if (urlMode) return 3;
  return COLS;
}

int KeyboardEntryActivity::getTotalRowCount() const { return getContentRowCount() + 1; }

bool KeyboardEntryActivity::isBottomRow(const int row) const { return row == getContentRowCount(); }

char KeyboardEntryActivity::getSelectedChar() const {
  const KeyDef(*layout)[COLS] = symMode ? symLayout : abcLayout;

  if (selectedRow < 0 || selectedRow >= getContentRowCount()) return '\0';
  if (selectedCol < 0 || selectedCol >= COLS) return '\0';

  const KeyDef& key = layout[selectedRow][selectedCol];
  return (shiftState > 0 && key.secondary != '\0') ? key.secondary : key.primary;
}

char KeyboardEntryActivity::getAlternativeChar() const {
  if (symMode || urlMode) return '\0';
  if (inputType == InputType::Url && selectedRow > 0) return '\0';

  const KeyDef(*layout)[COLS] = abcLayout;

  if (selectedRow < 0 || selectedRow >= getContentRowCount()) return '\0';
  if (selectedCol < 0 || selectedCol >= COLS) return '\0';

  const KeyDef& key = layout[selectedRow][selectedCol];
  const char current = getSelectedChar();
  if (current == key.primary && key.secondary != '\0') return key.secondary;
  if (current == key.secondary) return key.primary;
  return '\0';
}

bool KeyboardEntryActivity::insertChar(char c) {
  if (c == '\0') return true;
  if (maxLength != 0 && text.length() >= maxLength) return true;
  if (cursorPos > text.length()) cursorPos = text.length();

  text.insert(cursorPos, 1, c);
  cursorPos++;
  return true;
}

void KeyboardEntryActivity::insertString(const std::string& str) {
  if (str.empty()) return;
  if (maxLength != 0 && text.length() + str.length() > maxLength) return;
  if (cursorPos > text.length()) cursorPos = text.length();

  text.insert(cursorPos, str);
  cursorPos += str.length();
}

bool KeyboardEntryActivity::handleKeyPress() {
  if (isBottomRow(selectedRow)) {
    switch (static_cast<SpecialKeyType>(selectedCol)) {
      case SpecialKeyType::Shift:
        delPressCount = 0;
        hintVisible = false;
        if (urlMode || inputType == InputType::Url) return true;
        if (symMode) return true;
        shiftState = (shiftState + 1) % 2;
        return true;
      case SpecialKeyType::Mode: {
        delPressCount = 0;
        hintVisible = false;
        if (urlMode) {
          urlMode = false;
          symMode = false;
          selectedRow = getTotalRowCount() - 1;
          selectedCol = static_cast<int>(SpecialKeyType::Mode);
          requestUpdate();
          return true;
        }
        symMode = !symMode;
        int maxRow = getTotalRowCount() - 1;
        if (selectedRow > maxRow) selectedRow = maxRow;
        if (isBottomRow(selectedRow)) {
          if (selectedCol >= BOTTOM_KEY_COUNT) selectedCol = BOTTOM_KEY_COUNT - 1;
        } else {
          if (selectedCol >= getContentColCount()) selectedCol = getContentColCount() - 1;
        }
        return true;
      }
      case SpecialKeyType::Space:
        delPressCount = 0;
        hintVisible = false;
        if (inputType == InputType::Url) {
          urlMode = !urlMode;
          if (urlMode) {
            symMode = false;
          }
          selectedRow = getTotalRowCount() - 1;
          selectedCol = static_cast<int>(SpecialKeyType::Space);
          requestUpdate();
        } else {
          return insertChar(' ');
        }
        return true;
      case SpecialKeyType::Del:
        delPressCount++;
        if (delPressCount >= 2) {
          hintVisible = true;
          hintShowTime = millis();
        }
        if (cursorPos > 0 && !text.empty()) {
          text.erase(cursorPos - 1, 1);
          cursorPos--;
        }
        return true;
      case SpecialKeyType::Ok:
        delPressCount = 0;
        hintVisible = false;
        onComplete(text);
        return false;
      default:
        return true;
    }
  }

  if (urlMode) {
    delPressCount = 0;
    hintVisible = false;
    const int idx = selectedCol + selectedRow * 3;
    if (idx < URL_SNIPPET_COUNT) {
      insertString(urlSnippets[idx]);
    }
    return true;
  }

  delPressCount = 0;
  hintVisible = false;

  return insertChar(getSelectedChar());
}

void KeyboardEntryActivity::mapColContentBottom(int& col, bool goingUp) const {
  if (urlMode) {
    col = goingUp ? col - 1 : col + 1;
    if (col < 0) col = 0;
    if (col >= 3) col = 2;
  } else {
    col = goingUp ? col * 2 : col / 2;
  }
}

void KeyboardEntryActivity::loop() {
  const int totalRows = getTotalRowCount();

  if (!cursorMode && mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    upHeld = true;
    upLongHandled = false;
  }

  if (upHeld && !upLongHandled && mappedInput.isPressed(MappedInputManager::Button::Up) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    cursorMode = true;
    upLongHandled = true;
    hintVisible = true;
    hintShowTime = millis();
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (upHeld && !upLongHandled && !cursorMode) {
      bool wasBottom = isBottomRow(selectedRow);
      const int contentCols = getContentColCount();
      selectedRow = ButtonNavigator::previousIndex(selectedRow, totalRows);
      if (wasBottom && !isBottomRow(selectedRow)) {
        mapColContentBottom(selectedCol, true);
      } else if (!wasBottom && isBottomRow(selectedRow)) {
        mapColContentBottom(selectedCol, false);
      }
      int maxCol = isBottomRow(selectedRow) ? BOTTOM_KEY_COUNT - 1 : contentCols - 1;
      if (selectedCol > maxCol) selectedCol = maxCol;
      requestUpdate();
    }
    upHeld = false;
    upLongHandled = false;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    downHeld = true;
    if (cursorMode) {
      togglePos = false;
      passwordVisible = false;
      cursorMode = false;
      hintVisible = false;
      downLongHandled = true;
      requestUpdate();
    } else {
      downLongHandled = false;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (downHeld && !downLongHandled && !cursorMode) {
      bool wasBottom = isBottomRow(selectedRow);
      const int contentCols = getContentColCount();
      selectedRow = ButtonNavigator::nextIndex(selectedRow, totalRows);
      if (wasBottom && !isBottomRow(selectedRow)) {
        mapColContentBottom(selectedCol, true);
      } else if (!wasBottom && isBottomRow(selectedRow)) {
        mapColContentBottom(selectedCol, false);
      }
      int maxCol = isBottomRow(selectedRow) ? BOTTOM_KEY_COUNT - 1 : contentCols - 1;
      if (selectedCol > maxCol) selectedCol = maxCol;
      requestUpdate();
    }
    downHeld = false;
    downLongHandled = false;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] {
    if (cursorMode) return;
    int maxCol = isBottomRow(selectedRow) ? BOTTOM_KEY_COUNT - 1 : getContentColCount() - 1;
    selectedCol = ButtonNavigator::previousIndex(selectedCol, maxCol + 1);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (cursorMode) {
      if (togglePos) {
        cursorPos = savedCursorPos;
        togglePos = false;
        requestUpdate();
      } else if (cursorPos > 0) {
        cursorPos--;
        requestUpdate();
      }
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    if (cursorMode && inputType == InputType::Password && !togglePos) {
      rightHeld = true;
      rightLongHandled = false;
      rightStartCursorPos = cursorPos;
    }
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] {
    if (cursorMode) return;
    int maxCol = isBottomRow(selectedRow) ? BOTTOM_KEY_COUNT - 1 : getContentColCount() - 1;
    selectedCol = ButtonNavigator::nextIndex(selectedCol, maxCol + 1);
    requestUpdate();
  });

  if (rightHeld && !rightLongHandled && mappedInput.isPressed(MappedInputManager::Button::Right) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    if (cursorMode && inputType == InputType::Password && !togglePos) {
      savedCursorPos = rightStartCursorPos;
      togglePos = true;
      rightLongHandled = true;
      requestUpdate();
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (cursorMode && inputType == InputType::Password) {
      rightHeld = false;
      rightLongHandled = false;
    }
    if (cursorMode && !togglePos && cursorPos < text.length()) {
      cursorPos++;
      requestUpdate();
    }
    if (cursorMode) return;
    rightHeld = false;
    rightLongHandled = false;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    confirmHeld = true;
    confirmLongHandled = false;
  }

  if (confirmHeld && !confirmLongHandled && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() > DEL_LONG_PRESS_MS && isBottomRow(selectedRow) &&
      selectedCol == static_cast<int>(SpecialKeyType::Del)) {
    text.clear();
    cursorPos = 0;
    confirmLongHandled = true;
    requestUpdate();
  }

  if (confirmHeld && !confirmLongHandled && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    char alt = getAlternativeChar();
    if (alt != '\0') {
      insertChar(alt);
      requestUpdate();
      confirmLongHandled = true;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (confirmHeld && !confirmLongHandled && !cursorMode) {
      if (handleKeyPress()) {
        requestUpdate();
      }
    } else if (confirmHeld && !confirmLongHandled && cursorMode && inputType == InputType::Password && togglePos) {
      passwordVisible = !passwordVisible;
      requestUpdate();
    }
    confirmHeld = false;
    confirmLongHandled = false;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onCancel();
  }

  if (hintVisible && !cursorMode && millis() - hintShowTime > 4000) {
    hintVisible = false;
    requestUpdate();
  }
}

void KeyboardEntryActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title.c_str());

  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int inputStartY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing +
                          metrics.verticalSpacing * 4 + metrics.keyboardVerticalOffset;
  int inputHeight = 0;

  std::string displayText;
  if (inputType == InputType::Password && !passwordVisible) {
    size_t revealPos;
    if (cursorMode) {
      revealPos = text.length();  // no reveal in displayText; block draws actual char directly
    } else {
      revealPos = (text.length() > 0 && cursorPos > 0) ? cursorPos - 1 : std::string::npos;
    }
    displayText = text;
    for (size_t i = 0; i < displayText.length(); i++) {
      if (i != revealPos) {
        displayText[i] = '*';
      }
    }
  } else {
    displayText = text;
  }

  const bool isPassword = (inputType == InputType::Password);
  int availableWidth = pageWidth;
  if (gpio.deviceIsX3()) {
    availableWidth -= 2 * metrics.sideButtonHintsWidth;
  }
  const int effectiveMargin = (pageWidth - availableWidth * metrics.keyboardTextFieldWidthPercent / 100) / 2;
  const int toggleGap = isPassword ? 4 : 0;
  const int toggleReserve = isPassword ? std::max(renderer.getTextWidth(UI_12_FONT_ID, "[abc]"),
                                                  renderer.getTextWidth(UI_12_FONT_ID, "[***]")) +
                                             toggleGap
                                       : 0;
  const int textAreaWidth = pageWidth - 2 * effectiveMargin - toggleReserve;
  const int maxLineWidth = textAreaWidth;
  const bool centerText = metrics.keyboardCenteredText;

  int cursorCharWidth = 6;
  if (cursorPos < text.length()) {
    int w = renderer.getTextWidth(UI_12_FONT_ID, text.substr(cursorPos, 1).c_str());
    if (w > cursorCharWidth) cursorCharWidth = w;
  }

  int lineStartIdx = 0;
  int lineEndIdx = displayText.length();
  int textWidth = 0;
  int cursorPixelX = effectiveMargin;
  int cursorLineY = inputStartY;
  bool cursorDrawn = false;

  while (true) {
    std::string lineText = displayText.substr(lineStartIdx, lineEndIdx - lineStartIdx);
    textWidth = renderer.getTextWidth(UI_12_FONT_ID, lineText.c_str());
    if (textWidth <= maxLineWidth) {
      const bool isLastLine = (lineEndIdx == static_cast<int>(displayText.length()));
      bool isCursorLine = false;
      if (!cursorDrawn && cursorPos >= lineStartIdx &&
          (isLastLine ? cursorPos <= lineEndIdx : cursorPos < lineEndIdx)) {
        std::string beforeCursor;
        if (isPassword && !passwordVisible && cursorMode) {
          beforeCursor = std::string(cursorPos - lineStartIdx, '*');
        } else {
          beforeCursor = displayText.substr(lineStartIdx, cursorPos - lineStartIdx);
        }
        int beforeWidth = renderer.getTextWidth(UI_12_FONT_ID, beforeCursor.c_str());
        int kernOffset = 0;
        if (cursorPos < displayText.length()) {
          std::string beforeAndCursor = beforeCursor + displayText.substr(cursorPos, 1);
          int beforeAndCursorWidth = renderer.getTextWidth(UI_12_FONT_ID, beforeAndCursor.c_str());
          int charAdvance = renderer.getTextWidth(UI_12_FONT_ID, displayText.substr(cursorPos, 1).c_str());
          kernOffset = beforeAndCursorWidth - beforeWidth - charAdvance;
        }
        if (centerText) {
          cursorPixelX = effectiveMargin + (maxLineWidth - textWidth) / 2 + beforeWidth + kernOffset;
        } else {
          cursorPixelX = effectiveMargin + beforeWidth + kernOffset;
        }
        cursorLineY = inputStartY + inputHeight;
        cursorDrawn = true;
        isCursorLine = true;
      }

      const int lineStartX = centerText ? effectiveMargin + (maxLineWidth - textWidth) / 2 : effectiveMargin;
      if (isCursorLine && cursorMode && isPassword && !passwordVisible && !togglePos) {
        // Draw text in 3 parts to avoid block cursor overflowing onto next char.
        // displayText uses '*' for all chars; actual char may be wider than '*'.
        // Part 1: chars before cursor position
        const std::string part1 = displayText.substr(lineStartIdx, cursorPos - lineStartIdx);
        renderer.drawText(UI_12_FONT_ID, lineStartX, inputStartY + inputHeight, part1.c_str());
        // Part 2: skip cursor slot (block + actual char drawn later)
        // Part 3: chars after cursor position (skip char under cursor), starting at cursorPixelX + cursorCharWidth
        const int afterStart = static_cast<int>(cursorPos) + (cursorPos < text.length() ? 1 : 0);
        const int afterEnd = lineEndIdx;
        if (afterStart < afterEnd) {
          const std::string part3 = displayText.substr(afterStart, afterEnd - afterStart);
          renderer.drawText(UI_12_FONT_ID, cursorPixelX + cursorCharWidth, inputStartY + inputHeight, part3.c_str());
        }
      } else {
        renderer.drawText(UI_12_FONT_ID, lineStartX, inputStartY + inputHeight, lineText.c_str());
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

  const int fieldWidth = (inputHeight > 0) ? maxLineWidth : textWidth;
  const int lineMargin = effectiveMargin;
  GUI.drawTextField(renderer, Rect{0, inputStartY, pageWidth, inputHeight}, fieldWidth, cursorMode, lineMargin,
                    pageWidth - 2 * lineMargin);

  if (cursorMode && !togglePos && cursorPos <= displayText.length()) {
    static constexpr int blockPadding = 1;
    renderer.fillRect(cursorPixelX - blockPadding, cursorLineY, cursorCharWidth + blockPadding * 2, lineHeight, true);
    if (cursorPos < text.length()) {
      const char buf[2] = {text[cursorPos], '\0'};
      renderer.drawText(UI_12_FONT_ID, cursorPixelX, cursorLineY, buf, false);
    }
  } else if (cursorPos <= displayText.length()) {
    static constexpr int serifW = 3;
    const int cX = cursorPixelX;
    const int cY = cursorLineY;
    const int cBottom = cursorLineY + lineHeight - 1;
    renderer.fillRect(cX, cY, 2, lineHeight, true);
    renderer.drawLine(cX - serifW, cY, cX - 1, cY, 2, true);
    renderer.drawLine(cX + 1, cY, cX + serifW, cY, 2, true);
    renderer.drawLine(cX - serifW, cBottom, cX - 1, cBottom, 2, true);
    renderer.drawLine(cX + 1, cBottom, cX + serifW, cBottom, 2, true);
  }

  if (isPassword) {
    const char* toggleLabel = passwordVisible ? "[***]" : "[abc]";
    const int toggleWidth = renderer.getTextWidth(UI_12_FONT_ID, toggleLabel);
    const int toggleX = pageWidth - effectiveMargin - toggleWidth;
    const int toggleY = inputStartY + inputHeight;
    const bool toggleSelected = cursorMode && togglePos;

    if (toggleSelected) {
      renderer.fillRect(toggleX - 2, toggleY, toggleWidth + 5, lineHeight + 3, true);
      renderer.drawText(UI_12_FONT_ID, toggleX, toggleY, toggleLabel, false);
    } else {
      renderer.drawText(UI_12_FONT_ID, toggleX, toggleY, toggleLabel, true);
    }
  }

  if (hintVisible && !text.empty()) {
    const int hintLh = renderer.getLineHeight(SMALL_FONT_ID);
    const int underlineY = inputStartY + inputHeight + lineHeight + metrics.verticalSpacing;
    const int hintY = underlineY + 4;
    if (cursorMode) {
      int hintLineY = hintY;
      renderer.drawCenteredText(SMALL_FONT_ID, hintLineY, "Press < or > to move cursor", true);
      hintLineY += hintLh;
      if (inputType == InputType::Password) {
        const char* passTip;
        if (togglePos) {
          passTip = "Press < to return to cursor position";
        } else {
          passTip =
              passwordVisible ? "Hold > then press [***] to hide password" : "Hold > then press [abc] to show password";
        }
        renderer.drawCenteredText(SMALL_FONT_ID, hintLineY, passTip, true);
      }
    } else {
      renderer.drawCenteredText(SMALL_FONT_ID, hintY, "Hold UP to edit entry", true);
    }
  }

  const int keyHeight = metrics.keyboardKeyHeight;
  const int bottomKeyHeight = metrics.keyboardBottomKeyHeight;
  const int keySpacing = metrics.keyboardKeySpacing;
  const int contentCols = getContentColCount();
  const int keyboardWidth = pageWidth * metrics.keyboardWidthPercent / 100;
  const int keyWidth = (keyboardWidth - (contentCols - 1) * keySpacing) / contentCols;
  const int leftMargin = (pageWidth - (contentCols * keyWidth + (contentCols - 1) * keySpacing)) / 2;

  const int bottomRowGap = metrics.keyboardBottomKeySpacing > 0 ? 4 : 0;
  const int keyboardStartY = metrics.keyboardBottomAligned
                                 ? pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing -
                                       (keyHeight + keySpacing) * getContentRowCount() - bottomKeyHeight -
                                       bottomRowGap + metrics.keyboardVerticalOffset
                                 : inputStartY + inputHeight + lineHeight + metrics.verticalSpacing;

  const int tipsLh = renderer.getLineHeight(SMALL_FONT_ID);
  const int underlineBottom = inputStartY + inputHeight + lineHeight + metrics.verticalSpacing + 4;
  auto drawTip = [&](const char* tip, int y) { renderer.drawCenteredText(SMALL_FONT_ID, y, tip, true); };

  int tipCount = 0;
  if (cursorMode) {
    tipCount = 1;
  } else if (urlMode) {
    tipCount = 1 + (!text.empty() ? 1 : 0);
  } else if (symMode) {
    tipCount = !text.empty() ? 1 : 0;
  } else {
    tipCount = 1 + (inputType == InputType::Url ? 1 : 0) + (!text.empty() ? 1 : 0);
  }

  if (tipCount > 0) {
    int y = (underlineBottom + keyboardStartY) / 2 - (tipCount + 1) * tipsLh / 2;
    drawTip("Tips:", y);
    y += tipsLh;
    if (cursorMode) {
      drawTip("Press DOWN to return to keyboard", y);
    } else if (urlMode) {
      drawTip("Press ABC to exit URL mode", y);
      y += tipsLh;
      if (!text.empty()) {
        drawTip("Hold DEL to clear all text", y);
      }
    } else if (symMode) {
      if (!text.empty()) {
        drawTip("Hold DEL to clear all text", y);
      }
    } else {
      const char* altCharTip;
      if (inputType == InputType::Url) {
        altCharTip = "Hold SELECT for secondary char";
      } else if (shiftState > 0) {
        altCharTip = "Hold SELECT for lowercase or secondary char";
      } else {
        altCharTip = "Hold SELECT for UPPERCASE or secondary char";
      }
      drawTip(altCharTip, y);
      y += tipsLh;
      if (inputType == InputType::Url) {
        drawTip("Press URL for snippets", y);
        y += tipsLh;
      }
      if (!text.empty()) {
        drawTip("Hold DEL to clear all text", y);
      }
    }
  }

  const int bkSpacing = metrics.keyboardBottomKeySpacing;
  const int abcKeyWidth = (keyboardWidth - (COLS - 1) * keySpacing) / COLS;
  const int contentTotalWidth = COLS * abcKeyWidth + (COLS - 1) * keySpacing;
  const int bottomKeyWidth = (contentTotalWidth - (BOTTOM_KEY_COUNT - 1) * bkSpacing) / BOTTOM_KEY_COUNT;
  const int bottomLeftMargin =
      (pageWidth - (BOTTOM_KEY_COUNT * bottomKeyWidth + (BOTTOM_KEY_COUNT - 1) * bkSpacing)) / 2;

  int urlLeftMargin = leftMargin;
  if (urlMode) {
    const int urlTotalWidth = 3 * keyWidth + 2 * keySpacing;
    const int urlCenterX =
        bottomLeftMargin + static_cast<int>(SpecialKeyType::Space) * (bottomKeyWidth + bkSpacing) + bottomKeyWidth / 2;
    urlLeftMargin = urlCenterX - urlTotalWidth / 2;
  }

  const KeyDef(*layout)[COLS] = symMode ? symLayout : abcLayout;
  const int contentRows = getContentRowCount();

  for (int row = 0; row < contentRows; row++) {
    const int rowY = keyboardStartY + row * (keyHeight + keySpacing);
    const int rowLeftMargin = urlMode ? urlLeftMargin : leftMargin;

    for (int col = 0; col < contentCols; col++) {
      const int keyX = rowLeftMargin + col * (keyWidth + keySpacing);
      const bool isSelected = row == selectedRow && col == selectedCol;
      const bool activeKeySelected = isSelected && !cursorMode;

      if (urlMode) {
        const int snippetIdx = col + row * 3;
        if (snippetIdx < URL_SNIPPET_COUNT) {
          GUI.drawKeyboardKey(renderer, Rect{keyX, rowY, keyWidth, keyHeight}, urlSnippets[snippetIdx],
                              activeKeySelected, nullptr);
        }
      } else {
        const KeyDef& key = layout[row][col];

        char primaryChar = key.primary;
        char secondaryChar = key.secondary;

        if (!symMode && shiftState > 0 && key.secondary != '\0') {
          primaryChar = key.secondary;
          secondaryChar = key.primary;
        }

        const char primaryBuf[2] = {primaryChar, '\0'};
        const char secondaryBuf[2] = {secondaryChar, '\0'};
        const bool showSecondary = !symMode && row == 0 && secondaryChar != '\0';
        GUI.drawKeyboardKey(renderer, Rect{keyX, rowY, keyWidth, keyHeight}, primaryBuf, activeKeySelected,
                            showSecondary ? secondaryBuf : nullptr);
      }
    }
  }

  const int bottomRowY = keyboardStartY + contentRows * (keyHeight + keySpacing) + bottomRowGap;
  const bool bottomSelected = isBottomRow(selectedRow);

  struct BottomKeyInfo {
    KeyboardKeyType themeType;
    const char* label;
  };
  const BottomKeyInfo bottomKeys[BOTTOM_KEY_COUNT] = {
      {(symMode || urlMode || inputType == InputType::Url) ? KeyboardKeyType::Disabled : KeyboardKeyType::Shift,
       (symMode || urlMode || inputType == InputType::Url) ? shiftString[0] : shiftString[shiftState]},
      {KeyboardKeyType::Mode, urlMode ? "abc" : (symMode ? "abc" : "#@!")},
      {inputType == InputType::Url ? KeyboardKeyType::Mode : KeyboardKeyType::Space,
       inputType == InputType::Url ? "URL" : nullptr},
      {KeyboardKeyType::Del, nullptr},
      {KeyboardKeyType::Ok, tr(STR_OK_BUTTON)},
  };

  for (int i = 0; i < BOTTOM_KEY_COUNT; i++) {
    const int keyX = bottomLeftMargin + i * (bottomKeyWidth + bkSpacing);
    const bool isSelected = bottomSelected && i == selectedCol;

    const bool activeKeySelected = isSelected && !cursorMode;
    GUI.drawKeyboardKey(renderer, Rect{keyX, bottomRowY, bottomKeyWidth, bottomKeyHeight}, bottomKeys[i].label,
                        activeKeySelected, nullptr, bottomKeys[i].themeType);
  }

  if (cursorMode) {
    int selKeyX, selKeyY, selKeyW, selKeyH;
    if (isBottomRow(selectedRow)) {
      selKeyX = bottomLeftMargin + selectedCol * (bottomKeyWidth + bkSpacing);
      selKeyY = bottomRowY;
      selKeyW = bottomKeyWidth;
      selKeyH = bottomKeyHeight;
    } else {
      const int rowLM = urlMode ? urlLeftMargin : leftMargin;
      selKeyX = rowLM + selectedCol * (keyWidth + keySpacing);
      selKeyY = keyboardStartY + selectedRow * (keyHeight + keySpacing);
      selKeyW = keyWidth;
      selKeyH = keyHeight;
    }
    if (isBottomRow(selectedRow)) {
      GUI.drawKeyboardKey(renderer, Rect{selKeyX, selKeyY, selKeyW, selKeyH}, bottomKeys[selectedCol].label, true,
                          nullptr, bottomKeys[selectedCol].themeType, true);
    } else if (urlMode) {
      const int idx = selectedCol + selectedRow * 3;
      if (idx < URL_SNIPPET_COUNT) {
        GUI.drawKeyboardKey(renderer, Rect{selKeyX, selKeyY, selKeyW, selKeyH}, urlSnippets[idx], true, nullptr,
                            KeyboardKeyType::Normal, true);
      }
    } else {
      const KeyDef& selKey = layout[selectedRow][selectedCol];
      char selPrimary = selKey.primary;
      char selSecondary = selKey.secondary;
      if (!symMode && shiftState > 0 && selKey.secondary != '\0') {
        selPrimary = selKey.secondary;
        selSecondary = selKey.primary;
      }
      const char selPrimaryBuf[2] = {selPrimary, '\0'};
      const char selSecondaryBuf[2] = {selSecondary, '\0'};
      const bool selShowSecondary = !symMode && selectedRow == 0 && selSecondary != '\0';
      GUI.drawKeyboardKey(renderer, Rect{selKeyX, selKeyY, selKeyW, selKeyH}, selPrimaryBuf, true,
                          selShowSecondary ? selSecondaryBuf : nullptr, KeyboardKeyType::Normal, true);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  GUI.drawSideButtonHints(renderer, ">", "<");

  renderer.displayBuffer();
}

void KeyboardEntryActivity::onComplete(std::string text) {
  setResult(KeyboardResult{std::move(text)});
  finish();
}

void KeyboardEntryActivity::onCancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}
