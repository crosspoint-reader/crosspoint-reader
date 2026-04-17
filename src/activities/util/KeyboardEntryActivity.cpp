#include "KeyboardEntryActivity.h"

#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void KeyboardEntryActivity::onEnter() {
  Activity::onEnter();

  // Trigger first update
  requestUpdate();
}

void KeyboardEntryActivity::onExit() { Activity::onExit(); }

KeyboardEntryActivity::KeyBlock KeyboardEntryActivity::keyboard[] = {
  {.row = {"abc", "def", "ghi"}}, {.row = {"jkl", "mno", "pqr"}}, {.row = {"stu", "vwx", "yz`"}},
  {.row = {"ABC", "DEF", "GHI"}}, {.row = {"JKL", "MNO", "PQR"}}, {.row = {"STU", "VWX", "YZ~"}},
  {.row = {"123", "456", "789"}}, {.row = {"0!@", "#$%", "^&*"}}, {.row = {"()-", "_=+", "[]\\"}},
  {.row = {"{}|", ";:'", "\",."}}, {.row = {"/<>", "   ", "   "}}, {.row = {"   ", "   ", "   "}}
};

char KeyboardEntryActivity::getSelectedChar() const {
  return keyboard[selectedTopLevel + keyPage * 3].row[selectedMidLevel][selectedBottomLevel];
}

void KeyboardEntryActivity::resetLevels(){
  selectedTopLevel = -1;
  selectedMidLevel = -1;
  selectedBottomLevel = -1;
}

bool KeyboardEntryActivity::handleKeyPress() {
  if(selectedBottomLevel == -1){
    return false;
  }

  // Regular character
  const char c = getSelectedChar();
  if (c == '\0') {
    return true;
  }

  if (maxLength == 0 || text.length() < maxLength) {
    text += c;
    resetLevels();
  }

  return true;
}

void KeyboardEntryActivity::setLevelOnPress(int level) {
  if (selectedTopLevel == -1) {
    selectedTopLevel = level;
  } else if (selectedMidLevel == -1) {
    selectedMidLevel = level;
  } else {
    selectedBottomLevel = level;
  }
}

void KeyboardEntryActivity::loop() {
  // Handle navigation
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] {
    int pages = sizeof(KeyboardEntryActivity::keyboard) / (sizeof(KeyboardEntryActivity::KeyBlock) * 3);
    keyPage = (keyPage - 1 + pages) % pages;
    requestUpdate();
  });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] {
    int pages = sizeof(KeyboardEntryActivity::keyboard) / (sizeof(KeyboardEntryActivity::KeyBlock) * 3);
    keyPage = (keyPage + 1 + pages) % pages;
    requestUpdate();
  });

  buttonNavigator.onPress({MappedInputManager::Button::Left}, [this] {
    setLevelOnPress(1);
    handleKeyPress();
    requestUpdate();
  });

  buttonNavigator.onPress({MappedInputManager::Button::Right}, [this] {
    setLevelOnPress(2);
    handleKeyPress();
    requestUpdate();
  });

  // Selection
  buttonNavigator.onPress({MappedInputManager::Button::Confirm}, [this] {
    setLevelOnPress(0);
    handleKeyPress();
    requestUpdate();
  });

  // Cancel
  buttonNavigator.onPress({MappedInputManager::Button::Back}, [this] {
    if (selectedMidLevel != -1){
      selectedBottomLevel = -1;
      selectedMidLevel = -1;
    } else if (selectedTopLevel != -1) {
      resetLevels();
    } else {
      onCancel();
    }
    requestUpdate();
  });

  buttonNavigator.onContinuous({MappedInputManager::Button::Back}, [this] {
    onCancel();
  });

  buttonNavigator.onContinuous({MappedInputManager::Button::Confirm}, [this] {
    onComplete(text);
  });


  buttonNavigator.onContinuous({MappedInputManager::Button::Left}, [this] {
    if (!text.empty()) {
        text.pop_back();
        resetLevels();
        requestUpdate();
    }
  });

  buttonNavigator.onContinuous({MappedInputManager::Button::Right}, [this] {
    if (maxLength == 0 || text.length() <maxLength) {
        text += ' ';
        resetLevels();
        requestUpdate();
    }
  });
}

void KeyboardEntryActivity::render(RenderLock&&) {
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
  const int keyboardStartY = metrics.keyboardBottomAligned
                                   ? pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing -
                                         (metrics.keyboardKeyHeight + metrics.keyboardKeySpacing) * 4
                                   : inputStartY + inputHeight + metrics.verticalSpacing * 4;

  char buffer[] = "\0\0\0\0\0"; // Big enough for any single utf8 rune

  const int keyWidth = metrics.keyboardKeyWidth;
  const int keyHeight = metrics.keyboardKeyHeight;
  const int keySpacing = metrics.keyboardKeySpacing;
  const int width = keyWidth * 3 + keySpacing * 2;
  const int requiredSpace = pageWidth - width * 3;

  for(int i = 0; i < 3; i++){
    int x = (pageWidth - requiredSpace) / 3 * (i + 1);
    renderer.drawRect(x - 1, keyboardStartY, width, keyHeight * 3 + 1);
  }


  if (selectedTopLevel == -1) {
    for(int i = 0; i < 3; i++) {
      int x = (pageWidth - requiredSpace) / 3 * (i + 1);
      for(int row = 0; row < 3; row++){
        int startX = x;
        for(int j = 0; j < 3; j++){
          buffer[0] = keyboard[i + keyPage * 3].row[row][j];
          GUI.drawKeyboardKey(renderer, Rect{x, keyboardStartY + row * keyHeight, keyWidth, keyHeight}, buffer, false);
          x += keyWidth + keySpacing;
        }
        x = startX;
      }
    }
  } else if (selectedMidLevel == -1){
    for(int i = 0; i < 3; i++) {
      int x = (pageWidth - requiredSpace) / 3 * (i + 1);
      for(int j = 0; j < 3; j++){
        buffer[0] = keyboard[selectedTopLevel + keyPage * 3].row[i][j];
        GUI.drawKeyboardKey(renderer, Rect{x, keyboardStartY, keyWidth, keyHeight}, buffer, false);
        x += keyWidth + keySpacing;
      }
    }
  } else {
    for(int i = 0; i < 3; i++){
      int x = (pageWidth - requiredSpace) / 3 * (i + 1) + keyWidth + keySpacing;
      buffer[0] = keyboard[selectedTopLevel + keyPage * 3].row[selectedMidLevel][i];
      buffer[1] = '\0';
      GUI.drawKeyboardKey(renderer, Rect{x, keyboardStartY, keyWidth, keyHeight}, buffer, false);
    }
  }



  // Draw help text
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Draw side button hints for Up/Down navigation
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
