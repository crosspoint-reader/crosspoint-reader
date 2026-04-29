#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "activities/Activity.h"

class DictPrepareTask;

// Activity that performs one or more dictionary preparation steps:
//   1. Extract .dict.dz → .dict  (if .dict.dz present and .dict absent)
//   2. Extract .syn.dz  → .syn   (if .syn.dz  present and .syn  absent)
//   3. Generate .idx.oft from .idx  (if .idx present and .idx.oft absent)
//   4. Generate .syn.oft from .syn  (if .syn present and .syn.oft absent)
//
// Shows a confirmation screen listing required steps with time/charger warnings,
// then runs all steps sequentially on a FreeRTOS task with per-step progress bars.
// Returns isCancelled=false on success, true on cancel or any failure.
class DictPrepareActivity final : public Activity {
  friend class DictPrepareTask;

 public:
  explicit DictPrepareActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string folderPath);
  ~DictPrepareActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return state == State::PROCESSING; }
  bool preventAutoSleep() override { return state == State::PROCESSING; }
  void render(RenderLock&&) override;

 private:
  enum class State { CONFIRM, PROCESSING, SUCCESS, FAILED, CANCELLED };

  enum class StepType { EXTRACT_DICT, EXTRACT_SYN, GEN_IDX, GEN_SYN, GEN_CSPT };

  enum class StepStatus { PENDING, IN_PROGRESS, COMPLETE, FAILED };

  struct Step {
    StepType type;
    StepStatus status = StepStatus::PENDING;
    // Written by task, read by render — size_t writes are atomic on 32-bit RISC-V.
    volatile size_t progress = 0;
    volatile size_t total = 0;
  };

  State state = State::CONFIRM;
  std::string folderPath;
  char dictName[64] = {};

  // Set by loop() when the user presses Cancel during PROCESSING.
  // Checked by the FreeRTOS task at each vTaskDelay(1) yield point.
  volatile bool cancelRequested = false;

  Step steps[5];
  int stepCount = 0;
  volatile int currentStep = 0;

  // Completion flags set by the task, polled by loop().
  volatile bool prepareDone = false;
  volatile bool prepareSucceeded = false;

  std::unique_ptr<DictPrepareTask> task;

  void detectSteps();

  // Runs all steps sequentially; called on FreeRTOS task.
  void runSteps();

  // Decompress a gzip-compressed file. Returns true on success.
  bool extractFile(const char* dzPath, const char* outPath, Step& step);

  // Scan srcPath and write an .oft offset file to oftPath.
  // skipPerEntry: bytes after the null-terminated word to skip (8 for .idx, 4 for .syn).
  bool generateOft(const char* srcPath, const char* oftPath, uint8_t skipPerEntry, Step& step);

  // Read .idx.oft page boundaries, sample headword prefixes from .idx at stride 16,
  // and write a .idx.oft.cspt file for optimized lookup.
  bool generateCspt(const char* idxPath, const char* oftPath, const char* csptPath, Step& step);

  static const char* stepLabel(StepType type);
};
