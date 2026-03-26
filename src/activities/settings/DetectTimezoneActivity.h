#pragma once

#include <string>

#include "activities/Activity.h"

class DetectTimezoneActivity final : public Activity {
 public:
  explicit DetectTimezoneActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DetectTimezone", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { CONNECTING, DETECTING, SUCCESS, FAILED };
  State state = CONNECTING;
  std::string detectedTimezone;
  bool dstKnown = false;
  bool dstActive = false;

  void onWifiSelectionComplete(bool success);
  void onWifiSelectionCancelled();
  void performDetect();
  const char* dstStatusLabel() const;
};
