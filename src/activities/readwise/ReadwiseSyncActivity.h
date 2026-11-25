#pragma once

#include <memory>
#include <string>

#include "activities/Activity.h"

class ReadwiseSync;

/**
 * Runs one Readwise sync: Wi-Fi selection, then ReadwiseSync::step() from
 * loop() with a progress screen, then a summary. Back cancels between steps.
 */
class ReadwiseSyncActivity final : public Activity {
 public:
  ReadwiseSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~ReadwiseSyncActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == RUNNING; }

 private:
  enum State { WIFI_SELECTION, RUNNING, DONE, FAILED };

  void onWifiSelectionComplete(bool success);
  void startSync();
  std::string statusLine() const;

  State state = WIFI_SELECTION;
  std::string errorMessage;
  std::unique_ptr<ReadwiseSync> sync;
  std::string lastStatus;
};
