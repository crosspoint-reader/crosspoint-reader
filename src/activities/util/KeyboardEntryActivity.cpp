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

const char* KeyboardEntryActivity::keyboard[] = {
  "abcd", "efgh", "ijkl", "mnop", "qrst", "uvwx", "yz12", "3456",
  "7890", "~!@#", "$%^&", "*()_", "+:<>", "?   "
};

char KeyboardEntryActivity::getSelectedChar() const {
  if (selectedTopLevel == 2 && selectedMidLevel == 2) {
    return '\0';
  }

  int group = selectedTopLevel / 2 + selectedMidLevel; // Each page offsets the indices by one

  return keyboard[group][selectedBottomLevel];
}

bool KeyboardEntryActivity::handleKeyPress() {
  // Handle special row (bottom row with shift, space, backspace, done)
  if (selectedTopLevel == 2 && selectedMidLevel == 2) {
    switch (selectedBottomLevel) {
      case 0:
        // Shift toggle (0 = lower case, 1 = upper case, 2 = shift lock)
        shiftState = (shiftState + 1) % 3;
        return true;
      case 1:
        if (maxLength == 0 || text.length() < maxLength) {
          text += ' ';
        }
        return true;
      case 2:
        // Backspace
        if (!text.empty()) {
          text.pop_back();
        }
        return true;
      case 3:
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
    setLevelOnPress(0);
    requestUpdate();
  });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] {
    //selectedRow = ButtonNavigator::nextIndex(selectedRow, NUM_ROWS);
    setLevelOnPress(1);
    requestUpdate();
  });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] {
    setLevelOnPress(2);
    requestUpdate();
  });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] {
    if (selectedBottomLevel == -1) {
      setLevelOnPress(3);
    } else {
      int oldLevel = selectedMidLevel;
      if (selectedMidLevel != -1) {
        selectedMidLevel = -1;
      }
      if (oldLevel != -1) {
        selectedTopLevel = -1;
      }
    }
    requestUpdate();
  });

  // Selection
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (handleKeyPress()) {
      requestUpdate();
    }
    // If handleKeyPress returns false, it means onComplete was triggered, no update needed
  }

  // Cancel
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (selectedMidLevel != -1){
      selectedMidLevel = -1;
      selectedBottomLevel = -1;
    } else if (selectedTopLevel != -1) {
      selectedBottomLevel = -1;
      selectedMidLevel = -1;
      selectedTopLevel = -1;
    } else {
      onCancel();
    }
  }
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

  char buffer[] = "A B C D\0";
  int width = renderer.getTextWidth(UI_12_FONT_ID, buffer);
  int height = renderer.getTextHeight(UI_12_FONT_ID);

  if (selectedTopLevel == -1) {
    int requiredSpace = pageWidth - width * 3;
    int x = (pageWidth - requiredSpace) / 3;

    for(int i = 0; i < 3; i++) {
      renderer.drawRect(x, keyboardStartY, width, height * 3);
      for(int row = 0; row < 3; row++){

        for(int j = 0; j < 4; j++){
          buffer[j * 2] = keyboard[i * 3 + row][j];
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
      for(int j = 0; j < 4; j++){
        buffer[j * 2] = keyboard[selectedTopLevel + i][j];
      }
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
