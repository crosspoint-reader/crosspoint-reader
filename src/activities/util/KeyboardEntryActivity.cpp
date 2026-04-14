#include "KeyboardEntryActivity.h"

#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

const char* const KeyboardEntryActivity::shiftString[2] = {"shift", "SHIFT"};

void KeyboardEntryActivity::onEnter() {
  Activity::onEnter();
  cursorPos = text.length();
  requestUpdate();
}

void KeyboardEntryActivity::onExit() { Activity::onExit(); }

int KeyboardEntryActivity::getContentRowCount() const { return ABC_ROWS; }

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
  if (symMode) return '\0';

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

  text.insert(cursorPos, 1, c);
  cursorPos++;
  return true;
}

bool KeyboardEntryActivity::handleKeyPress() {
  if (isBottomRow(selectedRow)) {
    switch (static_cast<SpecialKeyType>(selectedCol)) {
      case SpecShift:
        if (symMode) return true;
        shiftState = (shiftState + 1) % 2;
        return true;
      case SpecMode: {
        symMode = !symMode;
        int maxRow = getTotalRowCount() - 1;
        if (selectedRow > maxRow) selectedRow = maxRow;
        if (isBottomRow(selectedRow)) {
          if (selectedCol >= BOTTOM_KEY_COUNT) selectedCol = BOTTOM_KEY_COUNT - 1;
        } else {
          if (selectedCol >= COLS) selectedCol = COLS - 1;
        }
        return true;
      }
      case SpecSpace:
        return insertChar(' ');
      case SpecDel:
        if (cursorPos > 0 && !text.empty()) {
          text.erase(cursorPos - 1, 1);
          cursorPos--;
        }
        return true;
      case SpecOk:
        onComplete(text);
        return false;
      default:
        return true;
    }
  }

  return insertChar(getSelectedChar());
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
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (upHeld && !upLongHandled && !cursorMode) {
      bool wasBottom = isBottomRow(selectedRow);
      selectedRow = ButtonNavigator::previousIndex(selectedRow, totalRows);
      if (wasBottom && !isBottomRow(selectedRow)) {
        selectedCol = selectedCol * 2;
      } else if (!wasBottom && isBottomRow(selectedRow)) {
        selectedCol = selectedCol / 2;
      }
      int maxCol = isBottomRow(selectedRow) ? BOTTOM_KEY_COUNT - 1 : COLS - 1;
      if (selectedCol > maxCol) selectedCol = maxCol;
      requestUpdate();
    }
    upHeld = false;
    upLongHandled = false;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    downHeld = true;
    if (cursorMode) {
      if (cursorPos > text.length()) {
        cursorPos = text.length();
      }
      passwordVisible = false;
      cursorMode = false;
      downLongHandled = true;
      requestUpdate();
    } else {
      downLongHandled = false;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (downHeld && !downLongHandled && !cursorMode) {
      bool wasBottom = isBottomRow(selectedRow);
      selectedRow = ButtonNavigator::nextIndex(selectedRow, totalRows);
      if (wasBottom && !isBottomRow(selectedRow)) {
        selectedCol = selectedCol * 2;
      } else if (!wasBottom && isBottomRow(selectedRow)) {
        selectedCol = selectedCol / 2;
      }
      int maxCol = isBottomRow(selectedRow) ? BOTTOM_KEY_COUNT - 1 : COLS - 1;
      if (selectedCol > maxCol) selectedCol = maxCol;
      requestUpdate();
    }
    downHeld = false;
    downLongHandled = false;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] {
    if (cursorMode) {
      if (cursorPos > 0) {
        cursorPos--;
        requestUpdate();
      }
      return;
    }
    int maxCol = isBottomRow(selectedRow) ? BOTTOM_KEY_COUNT - 1 : COLS - 1;
    selectedCol = ButtonNavigator::previousIndex(selectedCol, maxCol + 1);
    requestUpdate();
  });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] {
    if (cursorMode) {
      if (cursorPos < text.length()) {
        cursorPos++;
        requestUpdate();
      } else if (cursorPos == text.length() && inputType == InputType::Password) {
        cursorPos = text.length() + 1;
        requestUpdate();
      }
      return;
    }
    int maxCol = isBottomRow(selectedRow) ? BOTTOM_KEY_COUNT - 1 : COLS - 1;
    selectedCol = ButtonNavigator::nextIndex(selectedCol, maxCol + 1);
    requestUpdate();
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    confirmHeld = true;
    confirmLongHandled = false;
  }

  if (confirmHeld && !confirmLongHandled && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() > DEL_LONG_PRESS_MS && isBottomRow(selectedRow) && selectedCol == SpecDel) {
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
    } else if (confirmHeld && !confirmLongHandled && cursorMode && inputType == InputType::Password &&
               cursorPos > text.length()) {
      passwordVisible = !passwordVisible;
      requestUpdate();
    }
    confirmHeld = false;
    confirmLongHandled = false;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onCancel();
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
      revealPos = (text.length() > 0 && cursorPos > 0) ? cursorPos - 1 : 0;
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
  const int margin = metrics.contentSidePadding;
  const int extraMargin = 10;
  const int effectiveMargin = margin + extraMargin;
  const int toggleGap = isPassword ? 8 : 0;
  const int toggleReserve = isPassword ? std::max(renderer.getTextWidth(UI_12_FONT_ID, "[abc]"),
                                                  renderer.getTextWidth(UI_12_FONT_ID, "[***]")) +
                                             toggleGap
                                       : 0;
  const int textAreaWidth = pageWidth - 2 * effectiveMargin - toggleReserve;
  const int maxLineWidth = textAreaWidth;
  const bool centerText = metrics.keyboardCenteredText;

  int cursorCharWidth;
  if (cursorPos < text.length()) {
    cursorCharWidth = renderer.getTextWidth(UI_12_FONT_ID, text.substr(cursorPos, 1).c_str());
  } else {
    cursorCharWidth = 6;
  }

  int lineStartIdx = 0;
  int lineEndIdx = displayText.length();
  int textWidth = 0;
  int cursorPixelX = metrics.contentSidePadding;
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
        if (centerText) {
          cursorPixelX = effectiveMargin + (maxLineWidth - textWidth) / 2 + beforeWidth;
        } else {
          cursorPixelX = effectiveMargin + beforeWidth;
        }
        cursorLineY = inputStartY + inputHeight;
        cursorDrawn = true;
        isCursorLine = true;
      }

      const int lineStartX = centerText ? effectiveMargin + (maxLineWidth - textWidth) / 2 : effectiveMargin;
      if (isCursorLine && cursorMode && isPassword && !passwordVisible) {
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
  const int lineMargin = margin + extraMargin - 5;
  GUI.drawTextField(renderer, Rect{0, inputStartY, pageWidth, inputHeight}, fieldWidth, cursorMode, lineMargin,
                    pageWidth - 2 * lineMargin);

  if (cursorMode && cursorPos <= displayText.length()) {
    static constexpr int blockPadding = 1;
    renderer.fillRect(cursorPixelX - blockPadding, cursorLineY, cursorCharWidth + blockPadding * 2, lineHeight, true);
    if (cursorPos < text.length()) {
      const char buf[2] = {text[cursorPos], '\0'};
      renderer.drawText(UI_12_FONT_ID, cursorPixelX, cursorLineY, buf, false);
    }
  } else if (!cursorMode && cursorPos <= displayText.length()) {
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
    const bool toggleSelected = cursorMode && cursorPos > text.length();

    if (toggleSelected) {
      renderer.fillRect(toggleX - 2, toggleY, toggleWidth + 5, lineHeight + 3, true);
      renderer.drawText(UI_12_FONT_ID, toggleX, toggleY, toggleLabel, false);
    } else {
      renderer.drawText(UI_12_FONT_ID, toggleX, toggleY, toggleLabel, true);
    }
  }

  const int keyHeight = metrics.keyboardKeyHeight;
  const int bottomKeyHeight = metrics.keyboardBottomKeyHeight;
  const int keySpacing = metrics.keyboardKeySpacing;
  const int keyWidth = (pageWidth * 95 / 100 - (COLS - 1) * keySpacing) / COLS;
  const int leftMargin = (pageWidth - (COLS * keyWidth + (COLS - 1) * keySpacing)) / 2;

  const int bottomRowGap = metrics.keyboardBottomKeySpacing > 0 ? 4 : 0;
  const int keyboardStartY = metrics.keyboardBottomAligned
                                 ? pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing -
                                       (keyHeight + keySpacing) * getContentRowCount() - bottomKeyHeight -
                                       bottomRowGap + metrics.keyboardVerticalOffset
                                 : inputStartY + inputHeight + lineHeight + metrics.verticalSpacing;

  const KeyDef(*layout)[COLS] = symMode ? symLayout : abcLayout;
  const int contentRows = getContentRowCount();

  for (int row = 0; row < contentRows; row++) {
    const int rowY = keyboardStartY + row * (keyHeight + keySpacing);

    for (int col = 0; col < COLS; col++) {
      const KeyDef& key = layout[row][col];
      const int keyX = leftMargin + col * (keyWidth + keySpacing);
      const bool isSelected = row == selectedRow && col == selectedCol;

      char primaryChar = key.primary;
      char secondaryChar = key.secondary;

      if (!symMode && shiftState > 0 && key.secondary != '\0') {
        primaryChar = key.secondary;
        secondaryChar = key.primary;
      }

      const char primaryBuf[2] = {primaryChar, '\0'};
      const char secondaryBuf[2] = {secondaryChar, '\0'};
      const bool showSecondary = !symMode && row == 0 && secondaryChar != '\0';
      const bool activeKeySelected = isSelected && !cursorMode;
      GUI.drawKeyboardKey(renderer, Rect{keyX, rowY, keyWidth, keyHeight}, primaryBuf, activeKeySelected,
                          showSecondary ? secondaryBuf : nullptr);
    }
  }

  const int bottomRowY = keyboardStartY + contentRows * (keyHeight + keySpacing) + bottomRowGap;
  const int bkSpacing = metrics.keyboardBottomKeySpacing;
  const int contentTotalWidth = COLS * keyWidth + (COLS - 1) * keySpacing;
  const int bottomKeyWidth = (contentTotalWidth - (BOTTOM_KEY_COUNT - 1) * bkSpacing) / BOTTOM_KEY_COUNT;
  const bool bottomSelected = isBottomRow(selectedRow);

  struct BottomKeyInfo {
    KeyboardKeyType themeType;
    const char* label;
  };
  const BottomKeyInfo bottomKeys[BOTTOM_KEY_COUNT] = {
      {KeyboardKeyType::Shift, symMode ? shiftString[0] : shiftString[shiftState]},
      {KeyboardKeyType::Mode, symMode ? "abc" : "#@!"},
      {KeyboardKeyType::Space, nullptr},
      {KeyboardKeyType::Del, nullptr},
      {KeyboardKeyType::Ok, tr(STR_OK_BUTTON)},
  };

  for (int i = 0; i < BOTTOM_KEY_COUNT; i++) {
    const int keyX = leftMargin + i * (bottomKeyWidth + bkSpacing);
    const bool isSelected = bottomSelected && i == selectedCol;

    const bool activeKeySelected = isSelected && !cursorMode;
    GUI.drawKeyboardKey(renderer, Rect{keyX, bottomRowY, bottomKeyWidth, bottomKeyHeight}, bottomKeys[i].label,
                        activeKeySelected, nullptr, bottomKeys[i].themeType);
  }

  if (cursorMode) {
    int selKeyX, selKeyY, selKeyW, selKeyH;
    if (isBottomRow(selectedRow)) {
      selKeyX = leftMargin + selectedCol * (bottomKeyWidth + bkSpacing);
      selKeyY = bottomRowY;
      selKeyW = bottomKeyWidth;
      selKeyH = bottomKeyHeight;
    } else {
      selKeyX = leftMargin + selectedCol * (keyWidth + keySpacing);
      selKeyY = keyboardStartY + selectedRow * (keyHeight + keySpacing);
      selKeyW = keyWidth;
      selKeyH = keyHeight;
    }
    if (isBottomRow(selectedRow)) {
      GUI.drawKeyboardKey(renderer, Rect{selKeyX, selKeyY, selKeyW, selKeyH}, bottomKeys[selectedCol].label, true,
                          nullptr, bottomKeys[selectedCol].themeType, true);
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
