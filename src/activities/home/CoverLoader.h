#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cassert>
#include <vector>

struct RecentBook;
class Activity;

class CoverLoader {
  enum class State { Idle, Running, Stopping };
  enum class CoverResult { Skipped, Generated, Failed };

  SemaphoreHandle_t mutex;
  TaskHandle_t taskHandle = nullptr;
  TaskHandle_t callerTask = nullptr;
  State state = State::Idle;
  int processed = 0;
  std::vector<CoverResult> results;
  int lastMerged = 0;
  int coverHeight = 0;
  bool complete = false;

  const std::vector<RecentBook>* books = nullptr;
  Activity* owner = nullptr;

  static void taskTrampoline(void* param);
  void taskLoop();

 public:
  CoverLoader() : mutex(xSemaphoreCreateMutex()) { assert(mutex != nullptr); }
  ~CoverLoader() { vSemaphoreDelete(mutex); }
  CoverLoader(const CoverLoader&) = delete;
  CoverLoader& operator=(const CoverLoader&) = delete;

  void start(const std::vector<RecentBook>* recents, int height, Activity* owner);
  void stop();

  struct MergeStatus {
    bool changed = false;
    bool complete = true;
  };

  MergeStatus mergeResults(std::vector<RecentBook>& recents);
};
