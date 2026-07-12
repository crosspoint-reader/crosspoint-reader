#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class SteganographyActivity final : public Activity {
 public:
  explicit SteganographyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Steganography", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { MODE_SELECT, FILE_SELECT, EMBED_DONE, EXTRACT_RESULT };
  enum Mode { EMBED, EXTRACT };

  State state = MODE_SELECT;
  Mode mode = EMBED;

  static constexpr int MAX_FILES = 30;
  char fileNames[MAX_FILES][32];
  int fileCount = 0;
  int fileIndex = 0;

  char selectedPath[80];

  static constexpr int MAX_MSG_LEN = 2000;
  char messageBuffer[MAX_MSG_LEN + 1];
  int messageLen = 0;

  int scrollOffset = 0;
  int totalLines = 0;
  bool embedSuccess = false;
  bool extractFound = false;

  std::vector<std::string> cachedLines;
  bool linesCacheDirty = true;

  ButtonNavigator buttonNavigator;

  void loadFileList();
  bool embedMessage(const char* path, const char* msg, int len);
  bool extractMessage(const char* path);
  void doEmbed();
  void doExtract();
};
