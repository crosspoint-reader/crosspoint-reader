#include "KeyboardEntryActivity.h"

#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

const char* const KeyboardEntryActivity::shiftString[2] = {"shift", "SHIFT"};

void KeyboardEntryActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void KeyboardEntryActivity::onExit() { Activity::onExit(); }

int KeyboardEntryActivity::getContentRowCount() const { return symMode ? SYM_ROWS : ABC_ROWS; }

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

  text += c;
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
        if (!text.empty()) {
          text.pop_back();
        }
        return true;
      case SpecOk:
        onComplete(text);
        return false;
    }
  }

  return insertChar(getSelectedChar());
}

void KeyboardEntryActivity::loop() {
  const int totalRows = getTotalRowCount();

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this, totalRows] {
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
  });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this, totalRows] {
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
  });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] {
    int maxCol = isBottomRow(selectedRow) ? BOTTOM_KEY_COUNT - 1 : COLS - 1;
    selectedCol = ButtonNavigator::previousIndex(selectedCol, maxCol + 1);
    requestUpdate();
  });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] {
    int maxCol = isBottomRow(selectedRow) ? BOTTOM_KEY_COUNT - 1 : COLS - 1;
    selectedCol = ButtonNavigator::nextIndex(selectedCol, maxCol + 1);
    requestUpdate();
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    confirmHeld = true;
    confirmLongHandled = false;
  }

  if (confirmHeld && !confirmLongHandled && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    char alt = getAlternativeChar();
    if (alt != '\0') {
      insertChar(alt);
      requestUpdate();
    }
    confirmLongHandled = true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (confirmHeld && !confirmLongHandled) {
      if (handleKeyPress()) {
        requestUpdate();
      }
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
  if (isPassword) {
    displayText = std::string(text.length(), '*');
  } else {
    displayText = text;
  }

  displayText += "_";

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

  const int keyHeight = metrics.keyboardKeyHeight;
  const int keySpacing = metrics.keyboardKeySpacing;
  const int keyWidth = (pageWidth * 95 / 100 - (COLS - 1) * keySpacing) / COLS;
  const int leftMargin = (pageWidth - (COLS * keyWidth + (COLS - 1) * keySpacing)) / 2;

  const int keyboardStartY = metrics.keyboardBottomAligned
                                 ? pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing -
                                       (keyHeight + keySpacing) * getTotalRowCount() + metrics.keyboardVerticalOffset
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

      char primaryBuf[2] = {primaryChar, '\0'};
      char secondaryBuf[2] = {secondaryChar, '\0'};

      const bool showSecondary = !symMode && row == 0 && secondaryChar != '\0';
      GUI.drawKeyboardKey(renderer, Rect{keyX, rowY, keyWidth, keyHeight}, primaryBuf, isSelected,
                          showSecondary ? secondaryBuf : nullptr);
    }
  }

  const int bottomRowGap = metrics.keyboardBottomKeySpacing > 0 ? 4 : 0;
  const int bottomRowY = keyboardStartY + contentRows * (keyHeight + keySpacing) + bottomRowGap;
  const int bkSpacing = metrics.keyboardBottomKeySpacing;
  const int contentTotalWidth = COLS * keyWidth + (COLS - 1) * keySpacing;
  const int bottomKeyWidth = (contentTotalWidth - (BOTTOM_KEY_COUNT - 1) * bkSpacing) / BOTTOM_KEY_COUNT;
  const int bottomSelectedRow = isBottomRow(selectedRow);

  struct BottomKeyInfo {
    SpecialKeyType type;
    KeyboardKeyType themeType;
    const char* label;
  };
  const BottomKeyInfo bottomKeys[BOTTOM_KEY_COUNT] = {
      {SpecShift, KeyboardKeyType::Shift, symMode ? shiftString[0] : shiftString[shiftState]},
      {SpecMode, KeyboardKeyType::Mode, symMode ? "abc" : "#@!"},
      {SpecSpace, KeyboardKeyType::Space, nullptr},
      {SpecDel, KeyboardKeyType::Del, nullptr},
      {SpecOk, KeyboardKeyType::Ok, tr(STR_OK_BUTTON)},
  };

  for (int i = 0; i < BOTTOM_KEY_COUNT; i++) {
    const int keyX = leftMargin + i * (bottomKeyWidth + bkSpacing);
    const bool isSelected = bottomSelectedRow && i == selectedCol;

    GUI.drawKeyboardKey(renderer, Rect{keyX, bottomRowY, bottomKeyWidth, keyHeight}, bottomKeys[i].label, isSelected,
                        nullptr, bottomKeys[i].themeType);
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
