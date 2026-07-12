#pragma once
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class SdFileBrowserActivity final : public Activity {
 public:
  explicit SdFileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SdFileBrowser", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { BROWSING, VIEWING_FILE, CONFIRM_DELETE, FILE_INFO };

  struct FileEntry {
    std::string name;
    bool isDir;
    uint32_t size;
  };

  State state = BROWSING;
  std::string currentPath;
  std::vector<FileEntry> entries;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;

  std::string viewContent;
  int viewScrollOffset = 0;
  int viewTotalLines = 0;

  std::string deleteTargetPath;
  std::string deleteTargetName;

  void loadDirectory();
  void navigateInto(int index);
  void navigateUp();
  void viewFile(int index);
  void showFileInfo(int index);
  void deleteFile();

  static std::string formatSize(uint32_t bytes);
  static bool isTextFile(const std::string& name);
};
