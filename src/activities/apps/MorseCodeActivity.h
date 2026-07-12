#pragma once

#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class MorseCodeActivity final : public Activity {
 public:
  explicit MorseCodeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("MorseCode", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { MODE_SELECT, MORSE_ENCODE, MORSE_DECODE, REFERENCE };

  State state = MODE_SELECT;
  int modeIndex = 0;
  ButtonNavigator buttonNavigator;

  // Encode
  std::string inputText;
  std::string morseOutput;

  // Decode
  std::string currentMorseChar;  // dots and dashes for current char
  std::string decodedText;
  unsigned long lastKeyTime = 0;
  static constexpr unsigned long LONG_PRESS_MS = 400;

  // Morse lookup
  static const char* morseTable[36];  // A-Z, 0-9
  static const char morseChars[36];

  static std::string textToMorse(const std::string& text);
  static char morseToChar(const std::string& morse);
  void decodePendingChar();

  void renderModeSelect() const;
  void renderEncode() const;
  void renderDecode() const;
  void renderReference() const;
};
