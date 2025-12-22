#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"

enum FileType {
  F_DIRECTORY = 0,
  F_EPUB,
  F_TXT,
  F_BMP,
  F_FILE
};

struct FileInfo {
    std::string name;
    std::string basename;
    FileType type;
};

class GridBrowserActivity final : public Activity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  std::string basepath = "/";
  std::vector<FileInfo> files;
  int selectorIndex = 0;
  int previousSelectorIndex = -1;
  int page;
  bool updateRequired = false;
  bool renderRequired = false;
  const std::function<void(const std::string&)> onSelect;
  const std::function<void()> onGoHome;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render(bool clear) const;
  void update(bool render) const;
  void loadFiles();
  void drawSelectionRectangle(int tileIndex, bool black) const;

 public:
  explicit GridBrowserActivity(GfxRenderer& renderer, InputManager& inputManager,
                              const std::function<void(const std::string&)>& onSelect,
                              const std::function<void()>& onGoHome)
      : Activity("FileSelection", renderer, inputManager), onSelect(onSelect), onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
 private:
  static void sortFileList(std::vector<FileInfo>& strs);
};
