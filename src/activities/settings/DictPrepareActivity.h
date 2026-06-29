#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>

#include "activities/Activity.h"

class DictPrepareTask;

// Activity that performs one or more dictionary preparation steps:
//   1. Extract .dict.dz → .dict  (if .dict.dz present and .dict absent)
//   2. Extract .syn.dz  → .syn   (if .syn.dz  present and .syn  absent)
//   3. Generate .idx.fpi from .idx  (Fenced Prefix Index — the exact-match
//      lookup fast path, plus a per-group ordinal field used by
//      Dictionary::wordAtOrdinal/findSimilar).
//   4. Generate .syn.fpi from .syn
//
// Shows a confirmation screen listing required steps with time/charger warnings,
// then runs all steps sequentially on a FreeRTOS task with per-step progress bars.
// Returns isCancelled=false on success, true on cancel or any failure.
class DictPrepareActivity final : public Activity {
  friend class DictPrepareTask;

 public:
  explicit DictPrepareActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string folderPath,
                               bool forceRebuild = false);
  ~DictPrepareActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return state == State::PROCESSING; }
  bool preventAutoSleep() override { return state == State::PROCESSING; }
  void render(RenderLock&&) override;

 private:
  enum class State { CONFIRM, PROCESSING, SUCCESS, FAILED, CANCELLED };

  enum class StepType { EXTRACT_DICT, EXTRACT_SYN, GEN_FPI, GEN_SYN_FPI };

  enum class StepStatus { PENDING, IN_PROGRESS, COMPLETE, FAILED };

  struct Step {
    StepType type;
    std::atomic<StepStatus> status = StepStatus::PENDING;
    std::atomic<size_t> progress = 0;
    std::atomic<size_t> total = 0;
    std::atomic<unsigned long> startMs = 0;
    std::atomic<unsigned long> elapsedMs = 0;
  };

  State state = State::CONFIRM;
  std::string folderPath;
  bool forceRebuild = false;
  char dictName[64] = {};

  // Set by loop() when the user presses Cancel during PROCESSING.
  // Checked by the FreeRTOS task at each vTaskDelay(1) yield point.
  std::atomic<bool> cancelRequested = false;

  Step steps[4];
  int stepCount = 0;
  std::atomic<int> currentStep = 0;

  // Completion flags set by the task, polled by loop().
  std::atomic<bool> prepareDone = false;
  std::atomic<bool> prepareSucceeded = false;

  unsigned long prepareStartMs = 0;
  unsigned long prepareElapsedMs = 0;

  std::unique_ptr<DictPrepareTask> task;

  void detectSteps();
  bool updateProgressIfPercentChanged(Step& step, size_t progress, int& lastPercent);

  // Runs all steps sequentially; called on FreeRTOS task.
  void runSteps();

  // Decompress a gzip-compressed file. Returns true on success.
  bool extractFile(const char* dzPath, const char* outPath, Step& step);

  // Wraps Dictionary::generateFpi with this activity's Step progress/cancellation
  // plumbing. skipPerEntry: 8 for .idx, 4 for .syn (see Dictionary::generateFpi).
  bool generateFpi(const char* srcPath, const char* fpiPath, uint8_t skipPerEntry, Step& step);

  static const char* stepLabel(StepType type);
};
