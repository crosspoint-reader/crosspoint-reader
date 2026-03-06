#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Simple full-screen viewer for plain-text files (.log, .txt, .json, .bak).
// Reads up to MAX_BYTES from the file, wraps lines to screen width, and
// lets the user scroll up/down.  Back exits.
class FileViewerActivity final : public Activity {
 public:
  static constexpr size_t MAX_BYTES = 8192;

  explicit FileViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& filePath)
      : Activity("FileViewer", renderer, mappedInput), filePath(filePath) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string filePath;
  std::vector<std::string> lines;  // word-wrapped lines ready to render
  int scrollLine = 0;              // index of topmost visible line
  int linesPerPage = 0;
  ButtonNavigator buttonNavigator;

  void loadFile();
  std::string shortName() const;
};
