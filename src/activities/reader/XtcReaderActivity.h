/**
 * XtcReaderActivity.h
 *
 * XTC ebook reader activity for CrossPoint Reader
 * Displays pre-rendered XTC pages on e-ink display
 */

#pragma once

#include <Xtc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "activities/ActivityWithSubactivity.h"

class XtcReaderActivity final : public ActivityWithSubactivity {
  std::shared_ptr<Xtc> xtc;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  uint32_t currentPage = 0;
  int pagesUntilFullRefresh = 0;
  bool updateRequired = false;
  /*
   * Pre-allocated page buffer and its size.
   * Purpose: reserve one contiguous buffer early (in `onEnter`) sized to the
   * page bitmap so later renders avoid large `malloc` calls that can fail
   * due to heap fragmentation immediately after boot. If allocation fails
   * we fall back to per-render `malloc` and continue normally.
   *
   * Lifecycle: allocated in `onEnter()`, reused by `renderPage()` when it
   * fits, and freed in `onExit()`.
   */
  uint8_t* preallocPageBuffer = nullptr;
  size_t preallocPageBufferSize = 0;
  const std::function<void()> onGoBack;
  const std::function<void()> onGoHome;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
  void renderPage();
  void saveProgress() const;
  void loadProgress();

 public:
  explicit XtcReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Xtc> xtc,
                             const std::function<void()>& onGoBack, const std::function<void()>& onGoHome)
      : ActivityWithSubactivity("XtcReader", renderer, mappedInput),
        xtc(std::move(xtc)),
        onGoBack(onGoBack),
        onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
