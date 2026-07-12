#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class BLEHIDDevice;
class BLECharacteristic;
class BLEServer;

class BleKeyboardActivity final : public Activity {
 public:
  explicit BleKeyboardActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BleKeyboard", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  enum State {
    SELECT_SCRIPT,
    PREVIEW,
    ADVERTISING,
    PAIRED,
    EXECUTING,
    DONE
  };

  State state = SELECT_SCRIPT;
  ButtonNavigator buttonNavigator;

  // Script selection
  std::vector<std::string> scriptFiles;
  int selectedIndex = 0;

  // Script content
  std::string selectedScript;
  std::vector<std::string> scriptLines;
  int currentLine = 0;

  // BLE HID
  BLEServer* pServer = nullptr;
  BLEHIDDevice* pHid = nullptr;
  BLECharacteristic* pInputChar = nullptr;
  bool deviceConnected = false;

  // Execution
  unsigned long delayUntil = 0;
  std::string lastCommand;
  int repeatCount = 0;

  static constexpr const char* DUCKY_DIR = "/biscuit/ducky/";
  static constexpr const char* BLE_DEVICE_NAME = "biscuit. keyboard";

  void loadScriptList();
  void loadScriptContent(const std::string& path);
  void startAdvertising();
  void stopAdvertising();
  void executeCurrentLine();
  void executeLine(const std::string& line);

  // DuckyScript command handlers
  void sendString(const std::string& text);
  void sendKey(uint8_t keyCode, uint8_t modifiers = 0);
  void sendKeyCombo(uint8_t modifiers, uint8_t keyCode);
  void releaseKeys();

  // Character to HID scan code mapping
  static uint8_t charToKeyCode(char c);
  static uint8_t charToModifier(char c);
  static uint8_t specialKeyCode(const std::string& keyName);
  static uint8_t modifierBit(const std::string& modName);

  // BLE server callbacks
  class ServerCallbacks;
};
