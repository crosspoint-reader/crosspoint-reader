#pragma once
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class TaskManagerActivity final : public Activity {
 public:
  explicit TaskManagerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TaskManager", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { MEMORY_VIEW, SYSTEM_VIEW };
  State state = MEMORY_VIEW;
  ButtonNavigator buttonNavigator;
  unsigned long lastRefresh = 0;
  static constexpr unsigned long REFRESH_INTERVAL_MS = 2000;

  uint32_t freeHeap = 0;
  uint32_t minFreeHeap = 0;
  uint32_t largestFreeBlock = 0;
  uint32_t totalHeap = 0;

  void refreshData();
};
