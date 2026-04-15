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
  {.row = {"abc", "def", "ghi"}}, {.row = {"jkl", "mno", "pqr"}}, {.row = {"stu", "vwx", "yz1"}},
  {.row = {"234", "567", "890"}}, {.row = {"+:<", ">?!", "    "}}, {.row = {"   ", "   ", "   "}}
};

char KeyboardEntryActivity::getSelectedChar() const {
  return keyboard[selectedTopLevel].row[selectedMidLevel][selectedBottomLevel];
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
    selectedTopLevel = -1;
    selectedMidLevel = -1;
    selectedBottomLevel = -1;
    // Auto-disable shift after typing a character in non-lock mode
    if (shiftState == 1) {
      shiftState = 0;
    }
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
    requestUpdate();
  });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] {
    //selectedRow = ButtonNavigator::nextIndex(selectedRow, NUM_ROWS);
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
      selectedBottomLevel = -1;
      selectedMidLevel = -1;
      selectedTopLevel = -1;
    } else {
      onCancel();
    }
    requestUpdate();
  });

  buttonNavigator.onContinuous({MappedInputManager::Button::Back}, [this] {

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

  char buffer[] = "A B C\0";
  int width = renderer.getTextWidth(UI_12_FONT_ID, buffer);
  int height = renderer.getTextHeight(UI_12_FONT_ID);

  if (selectedTopLevel == -1) {
    int requiredSpace = pageWidth - width * 3;
    int x = (pageWidth - requiredSpace) / 3;

    for(int i = 0; i < 3; i++) {
      renderer.drawRect(x, keyboardStartY, width, height * 3);
      for(int row = 0; row < 3; row++){

        for(int j = 0; j < 3; j++){
          buffer[j * 2] = keyboard[i].row[row][j];
        }
        renderer.drawText(UI_12_FONT_ID, x + 1, keyboardStartY + 1 + row * height, buffer);
      }
      x += width + 10;
    }
  } else if (selectedMidLevel == -1){
    int requiredSpace = pageWidth - width * 3;
    int x = (pageWidth - requiredSpace) / 3;

    for(int i = 0; i < 3; i++) {
      renderer.drawRect(x, keyboardStartY, width, height * 2);
      for(int j = 0; j < 3; j++){
        buffer[j * 2] = keyboard[selectedTopLevel].row[i][j];
      }
      renderer.drawText(UI_12_FONT_ID, x + 1, keyboardStartY + 1, buffer);
      x += width + 10;
    }
  } else {
    int requiredSpace = pageWidth - width * 3;
    int x = (pageWidth - requiredSpace) / 4;
    for(int i = 0; i < 3; i++){
      renderer.drawRect(x, keyboardStartY, width, height * 2);
      buffer[0] = keyboard[selectedTopLevel].row[selectedMidLevel][i];
      buffer[1] = '\0';
      renderer.drawText(UI_12_FONT_ID, x + 1, keyboardStartY + 1, buffer);
      x += width + 10;
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
