#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class DiceRollerActivity final : public Activity {
 public:
  explicit DiceRollerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DiceRoller", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { SELECT, ROLLING, RESULT };

  State state = SELECT;

  // Die types: d4, d6, d8, d10, d12, d20, d100
  static constexpr int DIE_TYPES[] = {4, 6, 8, 10, 12, 20, 100};
  static constexpr int NUM_DIE_TYPES = 7;
  int dieTypeIndex = 1;  // default d6
  int dieCount = 1;      // 1-6 dice

  // Rolling animation
  int animFrame = 0;
  static constexpr int ANIM_FRAMES = 4;
  unsigned long animStartMs = 0;
  static constexpr unsigned long ANIM_FRAME_MS = 250;

  // Results
  std::vector<int> diceResults;
  int total = 0;

  void doRoll();
  void renderSelect() const;
  void renderRolling() const;
  void renderResult() const;

  static uint32_t randomRange(uint32_t max);
};
