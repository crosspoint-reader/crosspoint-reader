#pragma once

#include <string>

#include "activities/Activity.h"

class QrGeneratorActivity final : public Activity {
 public:
  explicit QrGeneratorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("QrGenerator", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { TEXT_INPUT, QR_DISPLAY };

  State state = TEXT_INPUT;
  std::string textPayload;
};
