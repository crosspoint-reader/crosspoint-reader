#pragma once
#include <cstdint>

#include "activities/Activity.h"

class OtpGeneratorActivity final : public Activity {
 public:
  explicit OtpGeneratorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("OtpGenerator", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr int BYTES_PER_PAGE = 100;
  static constexpr int COLS = 10;
  static constexpr int ROWS = 10;

  uint8_t pageData[BYTES_PER_PAGE] = {};
  int pageNumber = 0;

  void generatePage();
};
