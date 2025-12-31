#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>

#include "../Activity.h"

class HomeActivity final : public Activity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int selectorIndex = 0;
  bool updateRequired = false;
  mutable bool isFirstRender = true;
  bool hasContinueReading = false;
  std::string lastBookTitle;
  std::string lastBookAuthor;
  const std::function<void()> onContinueReading;
  const std::function<void()> onReaderOpen;
  const std::function<void()> onSettingsOpen;
  const std::function<void()> onFileTransferOpen;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  int getMenuItemCount() const;

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        const std::function<void()>& onContinueReading, const std::function<void()>& onReaderOpen,
                        const std::function<void()>& onSettingsOpen, const std::function<void()>& onFileTransferOpen)
      : Activity("Home", renderer, mappedInput),
        onContinueReading(onContinueReading),
        onReaderOpen(onReaderOpen),
        onSettingsOpen(onSettingsOpen),
        onFileTransferOpen(onFileTransferOpen) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
