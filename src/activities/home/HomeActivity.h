#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"

// Cached data for a recent book
struct CachedBookInfo {
  std::string path;       // Full path to the book file
  std::string title;
  std::string coverPath;
  int progressPercent = 0;
};

class HomeActivity final : public Activity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int selectorIndex = 0;      // Menu item index
  int bookSelectorIndex = 0;  // Book selection index (0-2 for recent books)
  bool inBookSelection = true; // True = selecting books, False = selecting menu
  bool updateRequired = false;
  bool hasContinueReading = false;
  bool hasOpdsUrl = false;
  bool hasCoverImage = false;
  bool coverRendered = false;     // Track if cover has been rendered once
  bool coverBufferStored = false; // Track if cover buffer is stored
  uint8_t *coverBuffer = nullptr; // HomeActivity's own buffer for cover image
  std::string lastBookTitle;
  std::string lastBookAuthor;
  std::string coverBmpPath;
  uint8_t cachedBatteryLevel = 0;
  uint32_t lastBatteryCheck = 0;
  
  // Cached recent books data (loaded once in onEnter)
  std::vector<CachedBookInfo> cachedRecentBooks;
  const std::function<void()> onContinueReading;
  const std::function<void()> onMyLibraryOpen;
  const std::function<void()> onSettingsOpen;
  const std::function<void()> onFileTransferOpen;
  const std::function<void()> onOpdsBrowserOpen;

  static void taskTrampoline(void *param);
  [[noreturn]] void displayTaskLoop();
  void render();
  int getMenuItemCount() const;
  bool storeCoverBuffer();   // Store frame buffer for cover image
  bool restoreCoverBuffer(); // Restore frame buffer from stored cover
  void freeCoverBuffer();    // Free the stored cover buffer
  void loadRecentBooksData(); // Load and cache recent books data

public:
  explicit HomeActivity(GfxRenderer &renderer, MappedInputManager &mappedInput,
                        const std::function<void()> &onContinueReading,
                        const std::function<void()> &onMyLibraryOpen,
                        const std::function<void()> &onSettingsOpen,
                        const std::function<void()> &onFileTransferOpen,
                        const std::function<void()> &onOpdsBrowserOpen)
      : Activity("Home", renderer, mappedInput),
        onContinueReading(onContinueReading), onMyLibraryOpen(onMyLibraryOpen),
        onSettingsOpen(onSettingsOpen), onFileTransferOpen(onFileTransferOpen),
        onOpdsBrowserOpen(onOpdsBrowserOpen) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
