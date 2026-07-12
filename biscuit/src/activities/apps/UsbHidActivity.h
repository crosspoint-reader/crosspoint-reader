#pragma once
#include <string>
#include <vector>
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class UsbHidActivity final : public Activity {
 public:
  explicit UsbHidActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("UsbHid", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == RUNNING; }
  bool skipLoopDelay() override { return state == RUNNING; }

 private:
  enum State { FILE_SELECT, PREVIEW, CONFIRM_RUN, RUNNING, DONE, ERROR };
  State state = FILE_SELECT;

  // DuckyScript engine
  struct DuckyLine {
    enum Cmd {
      STRING, DELAY, KEY, COMBO, ENTER, TAB, SPACE, BACKSPACE, ESCAPE,
      ARROW_UP, ARROW_DOWN, ARROW_LEFT, ARROW_RIGHT,
      F_KEY, GUI_KEY, ALT_KEY, CTRL_KEY, SHIFT_KEY,
      REPEAT, REM
    };
    Cmd cmd = REM;
    char payload[128] = {};
    int value = 0;       // delay ms, F-key number, repeat count
    uint8_t modifier = 0;
  };

  std::vector<DuckyLine> script;
  int currentLine = 0;
  unsigned long lineStartTime = 0;
  bool waiting = false;
  int totalKeystrokes = 0;
  unsigned long runStartTime = 0;

  // File selection
  struct DuckyFile {
    char name[48];
  };
  std::vector<DuckyFile> duckyFiles;
  int fileIndex = 0;
  ButtonNavigator buttonNavigator;

  // Preview
  std::vector<std::string> previewLines;
  int previewScroll = 0;

  char errorMsg[64] = {};

  void loadDuckyFiles();
  bool parseScript(const char* path);
  void executeNextLine();
  DuckyLine parseLine(const char* line) const;

  // USB HID interface — MANUAL IMPLEMENTATION
  void initUsbHid();
  void sendKeystroke(uint8_t keycode, uint8_t modifier);
  void sendString(const char* str);
  void releaseAllKeys();
  void deinitUsbHid();

  // Key name to HID keycode mapping
  static uint8_t keyNameToHidCode(const char* name);
  static uint8_t charToHidCode(char c, uint8_t& modifier);
};
