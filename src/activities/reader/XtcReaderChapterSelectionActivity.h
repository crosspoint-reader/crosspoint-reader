#pragma once
#include <Xtc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <memory>

#include "../Activity.h"

class XtcReaderChapterSelectionActivity final : public Activity {
  std::shared_ptr<Xtc> xtc;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  uint32_t currentPage = 0;
  int selectorIndex = 0;
  bool updateRequired = false;
  const std::function<void()> onGoBack;
  const std::function<void(uint32_t newPage)> onSelectPage;

  int getPageItems() const;
  int findChapterIndexForPage(uint32_t page) const;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();

 public:
  explicit XtcReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const std::shared_ptr<Xtc>& xtc, uint32_t currentPage,
                                             const std::function<void()>& onGoBack,
                                             const std::function<void(uint32_t newPage)>& onSelectPage)
      : Activity("XtcReaderChapterSelection", renderer, mappedInput),
        xtc(xtc),
        currentPage(currentPage),
        onGoBack(onGoBack),
        onSelectPage(onSelectPage) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
