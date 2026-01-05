#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"

class RecentBooksActivity final : public Activity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int selectorIndex = 0;
  bool updateRequired = false;
  std::vector<std::string> bookTitles;  // Display titles for each book
  const std::function<void()> onGoBack;
  const std::function<void(const std::string& path)> onSelectBook;

  // Number of items that fit on a page
  int getPageItems() const;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                               const std::function<void()>& onGoBack,
                               const std::function<void(const std::string& path)>& onSelectBook)
      : Activity("RecentBooks", renderer, mappedInput), onGoBack(onGoBack), onSelectBook(onSelectBook) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
