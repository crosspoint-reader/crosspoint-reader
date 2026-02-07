#pragma once
#include <Fb2.h>
#include <Fb2/Fb2Section.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "EpubReaderMenuActivity.h"
#include "activities/ActivityWithSubactivity.h"

class Fb2ReaderActivity final : public ActivityWithSubactivity {
  std::shared_ptr<Fb2> fb2;
  std::unique_ptr<Fb2Section> section = nullptr;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int currentSectionIndex = 0;
  int nextPageNumber = 0;
  int pagesUntilFullRefresh = 0;
  int cachedSectionIndex = 0;
  int cachedSectionTotalPageCount = 0;
  bool pendingPercentJump = false;
  float pendingSpineProgress = 0.0f;
  bool updateRequired = false;
  bool pendingSubactivityExit = false;
  bool pendingGoHome = false;
  bool skipNextButtonCheck = false;
  const std::function<void()> onGoBack;
  const std::function<void()> onGoHome;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar(int orientedMarginRight, int orientedMarginBottom, int orientedMarginLeft) const;
  void saveProgress(int sectionIndex, int currentPage, int pageCount);
  void jumpToPercent(int percent);
  void onReaderMenuBack(uint8_t orientation);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void applyOrientation(uint8_t orientation);

 public:
  explicit Fb2ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Fb2> fb2,
                             const std::function<void()>& onGoBack, const std::function<void()>& onGoHome)
      : ActivityWithSubactivity("Fb2Reader", renderer, mappedInput),
        fb2(std::move(fb2)),
        onGoBack(onGoBack),
        onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
