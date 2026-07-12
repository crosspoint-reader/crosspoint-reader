#include "MorseCodeActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cctype>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

// Morse code lookup: A-Z (indices 0-25), 0-9 (indices 26-35)
const char* MorseCodeActivity::morseTable[36] = {
    ".-",    "-...",  "-.-.",  "-..",  ".",    "..-.", "--.",   "....",  "..",     // A-I
    ".---",  "-.-",   ".-..",  "--",   "-.",   "---",  ".--.",  "--.-",  ".-.",   // J-R
    "...",   "-",     "..-",   "...-", ".--",  "-..-", "-.--",  "--..",           // S-Z
    "-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", // 0-8
    "----."                                                                        // 9
};

const char MorseCodeActivity::morseChars[36] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L',
                                                 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
                                                 'Y', 'Z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};

std::string MorseCodeActivity::textToMorse(const std::string& text) {
  std::string result;
  for (char c : text) {
    if (c == ' ') {
      result += "/ ";
      continue;
    }
    char upper = toupper(c);
    for (int i = 0; i < 36; i++) {
      if (morseChars[i] == upper) {
        if (!result.empty() && result.back() != ' ') result += ' ';
        result += morseTable[i];
        break;
      }
    }
  }
  return result;
}

char MorseCodeActivity::morseToChar(const std::string& morse) {
  for (int i = 0; i < 36; i++) {
    if (morse == morseTable[i]) {
      return morseChars[i];
    }
  }
  return '?';
}

void MorseCodeActivity::decodePendingChar() {
  if (!currentMorseChar.empty()) {
    decodedText += morseToChar(currentMorseChar);
    currentMorseChar.clear();
    requestUpdate();
  }
}

void MorseCodeActivity::onEnter() {
  Activity::onEnter();
  state = MODE_SELECT;
  modeIndex = 0;
  inputText.clear();
  morseOutput.clear();
  decodedText.clear();
  currentMorseChar.clear();
  requestUpdate();
}

void MorseCodeActivity::onExit() { Activity::onExit(); }

void MorseCodeActivity::loop() {
  if (state == MODE_SELECT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    buttonNavigator.onNext([this] {
      modeIndex = ButtonNavigator::nextIndex(modeIndex, 3);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      modeIndex = ButtonNavigator::previousIndex(modeIndex, 3);
      requestUpdate();
    });
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (modeIndex == 0) {
        // Encode: get text input
        startActivityForResult(
            std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Text to Morse"),
            [this](const ActivityResult& result) {
              if (result.isCancelled) {
                state = MODE_SELECT;
                requestUpdate();
              } else {
                inputText = std::get<KeyboardResult>(result.data).text;
                morseOutput = textToMorse(inputText);
                state = MORSE_ENCODE;
                requestUpdate();
              }
            });
      } else if (modeIndex == 1) {
        state = MORSE_DECODE;
        decodedText.clear();
        currentMorseChar.clear();
        requestUpdate();
      } else {
        state = REFERENCE;
        requestUpdate();
      }
    }
    return;
  }

  if (state == MORSE_ENCODE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = MODE_SELECT;
      requestUpdate();
    }
    return;
  }

  if (state == MORSE_DECODE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      decodePendingChar();
      state = MODE_SELECT;
      requestUpdate();
      return;
    }

    // Confirm: short press = dot, long press = dash
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      unsigned long held = mappedInput.getHeldTime();
      if (held >= LONG_PRESS_MS) {
        currentMorseChar += '-';
      } else {
        currentMorseChar += '.';
      }
      lastKeyTime = millis();
      requestUpdate();
    }

    // Right = word space
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      decodePendingChar();
      decodedText += ' ';
      requestUpdate();
    }

    // Up = decode current morse char (letter space)
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      decodePendingChar();
    }

    // Down = backspace
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      if (!currentMorseChar.empty()) {
        currentMorseChar.pop_back();
      } else if (!decodedText.empty()) {
        decodedText.pop_back();
      }
      requestUpdate();
    }

    // Auto-decode after pause
    if (!currentMorseChar.empty() && lastKeyTime > 0 && millis() - lastKeyTime > 1500) {
      decodePendingChar();
      lastKeyTime = 0;
    }
    return;
  }

  if (state == REFERENCE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = MODE_SELECT;
      requestUpdate();
    }
  }
}

void MorseCodeActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_MORSE_CODE));

  switch (state) {
    case MODE_SELECT:
      renderModeSelect();
      break;
    case MORSE_ENCODE:
      renderEncode();
      break;
    case MORSE_DECODE:
      renderDecode();
      break;
    case REFERENCE:
      renderReference();
      break;
  }

  renderer.displayBuffer();
}

void MorseCodeActivity::renderModeSelect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  const char* modes[] = {tr(STR_ENCODE_TEXT), tr(STR_DECODE_MORSE), tr(STR_REFERENCE_CHART)};
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, 3, modeIndex,
      [&modes](int i) { return std::string(modes[i]); });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void MorseCodeActivity::renderEncode() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageHeight = renderer.getScreenHeight();
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;

  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, inputText.c_str());
  y += lineH + 20;

  // Word-wrap morse output
  auto lines = renderer.wrappedText(UI_10_FONT_ID, morseOutput.c_str(),
                                    renderer.getScreenWidth() - metrics.contentSidePadding * 2, 12);
  for (auto& line : lines) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, line.c_str());
    y += lineH;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void MorseCodeActivity::renderDecode() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageHeight = renderer.getScreenHeight();
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;

  // Current morse input
  std::string morseLabel = "Morse: " + currentMorseChar + "_";
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, morseLabel.c_str());
  y += lineH + 15;

  // Decoded text
  renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, "Decoded:", true, EpdFontFamily::BOLD);
  y += lineH + 5;

  auto lines = renderer.wrappedText(UI_10_FONT_ID, decodedText.c_str(),
                                    renderer.getScreenWidth() - metrics.contentSidePadding * 2, 8);
  for (auto& line : lines) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, line.c_str());
    y += lineH;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "./--", "Space", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, "Letter", "Del");
}

void MorseCodeActivity::renderReference() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int lineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int colWidth = pageWidth / 3;

  for (int i = 0; i < 36; i++) {
    int col = i / 13;
    int row = i % 13;
    int x = metrics.contentSidePadding + col * colWidth;
    int cy = y + row * lineH;

    char buf[16];
    snprintf(buf, sizeof(buf), "%c  %s", morseChars[i], morseTable[i]);
    renderer.drawText(SMALL_FONT_ID, x, cy, buf);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
