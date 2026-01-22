#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"

class MyLibraryActivity final : public Activity {
 public:
  enum class Tab { Recent, Files };

 private:
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;

  Tab currentTab = Tab::Recent;
  int selectorIndex = 0;
  bool updateRequired = false;

  // Recent tab state
  std::vector<std::string> bookTitles;  // Display titles for each book
  std::vector<std::string> bookPaths;   // Paths for each visible book (excludes missing)

  // Files tab state (from FileSelectionActivity)
  std::string basepath = "/";
  std::vector<std::string> files;

  // Callbacks
  const std::function<void()> onGoHome;
  const std::function<void(const std::string& path, Tab fromTab)> onSelectBook;

  // Number of items that fit on a page
  int getPageItems() const;
  int getCurrentItemCount() const;
  int getTotalPages() const;
  int getCurrentPage() const;

  // Data loading
  void loadRecentBooks();
  void loadFiles();
  size_t findEntry(const std::string& name) const;

  // Rendering
  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void renderRecentTab() const;
  void renderFilesTab() const;

 public:
  explicit MyLibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                             const std::function<void()>& onGoHome,
                             const std::function<void(const std::string& path, Tab fromTab)>& onSelectBook,
                             Tab initialTab = Tab::Recent, std::string initialPath = "/")
      : Activity("MyLibrary", renderer, mappedInput),
        currentTab(initialTab),
        basepath(initialPath.empty() ? "/" : std::move(initialPath)),
        onGoHome(onGoHome),
        onSelectBook(onSelectBook) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
