#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class NoteEditorActivity final : public Activity {
 public:
  explicit NoteEditorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path, bool createdNow = false)
      : Activity("NoteEditor", renderer, mappedInput), path_(std::move(path)), createdNow_(createdNow) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return true; }

 private:
  std::string path_;
  std::string text_;
  size_t cursor_ = 0;
  bool createdNow_ = false;
  bool dirty_ = false;
  bool savedOnce_ = false;
  bool bleStarted_ = false;
  bool confirmHeld_ = false;
  bool closeRequested_ = false;
  unsigned long lastAutosaveMs_ = 0;
  std::string status_;

  bool load();
  bool save();
  void insertChar(char ch);
  void insertText(const char* s);
  void backspace();
  void moveCursorLeft();
  void moveCursorRight();
  void handleBleKeys();
  void ensureBleConnected();
  std::vector<std::string> visibleLines(int maxLines) const;
};
