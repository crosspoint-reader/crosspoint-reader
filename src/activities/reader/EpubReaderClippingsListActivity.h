#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <vector>

#include "../ActivityWithSubactivity.h"
#include "ClippingStore.h"

class EpubReaderClippingsListActivity final : public ActivityWithSubactivity {
  std::string bookPath;
  std::vector<ClippingEntry> clippings;
  std::vector<std::string> previewCache;  // Cached preview strings to avoid SD reads during render
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int selectorIndex = 0;
  bool updateRequired = false;
  bool confirmingDelete = false;

  void refreshPreviews();

  const std::function<void()> onGoBack;

  int getPageItems() const;
  int getTotalItems() const;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();

 public:
  explicit EpubReaderClippingsListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& bookPath, const std::function<void()>& onGoBack)
      : ActivityWithSubactivity("EpubReaderClippingsList", renderer, mappedInput),
        bookPath(bookPath),
        onGoBack(onGoBack) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
