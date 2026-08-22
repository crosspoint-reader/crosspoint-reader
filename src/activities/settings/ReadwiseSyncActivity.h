#pragma once

#include <string>

#include "activities/Activity.h"

/**
 * Downloads Readwise Reader documents carrying the configured tag as EPUBs
 * into /Readwise. Page-based listing keeps only ~100 document entries in RAM;
 * already-downloaded articles (matched by filename) are skipped.
 */
class ReadwiseSyncActivity final : public Activity {
 public:
  explicit ReadwiseSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadwiseSync", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }
  bool preventAutoSleep() override { return state_ == SYNCING; }
  void render(RenderLock&&) override;

 private:
  enum State { SYNCING, SUCCESS, NO_CREDENTIALS, FAILED, CANCELLED };

  void performSync();
  bool downloadOne(const char* id, const char* title);
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);

  State state_ = SYNCING;
  std::string statusLine_;
  std::string errorDetail_;
  int downloadedCount_ = 0;
  int skippedCount_ = 0;
  bool started_ = false;
  bool cancelRequested_ = false;
  bool shouldTearDownWifiOnExit_ = false;
};
