#pragma once
#include <cstdint>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class GameOfLifeActivity final : public Activity {
 public:
  explicit GameOfLifeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("GameOfLife", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return running; }

 private:
  static constexpr int COLS = 48;
  static constexpr int ROWS = 80;

  uint8_t grid[COLS][ROWS]{};
  uint8_t nextGrid[COLS][ROWS]{};

  int cursorX = COLS / 2;
  int cursorY = ROWS / 2;
  bool running = false;
  int generation = 0;
  int population = 0;
  static constexpr int POP_HISTORY_SIZE = 40;
  int popHistory[POP_HISTORY_SIZE]{};
  int popHistoryIdx = 0;
  int popHistoryMax = 1;
  unsigned long lastStepTime = 0;
  static constexpr unsigned long STEP_INTERVAL_MS = 200;

  void randomize();
  void step();
  int countNeighbors(int x, int y) const;
  int countPopulation() const;

  // Pattern library extension
  enum ExtState { RUNNING_SIM, PATTERN_SELECT };
  ExtState extState = RUNNING_SIM;
  int patternIndex = 0;

  struct PatternDef {
    const char* name;
    const char* description;
    int width;
    int height;
    const uint8_t* data;  // packed bits, row-major, MSB first
  };
  static const PatternDef PATTERNS[];
  static constexpr int PATTERN_COUNT = 12;

  ButtonNavigator buttonNavigator;

  void loadPattern(int index);
  void placePatternAt(const PatternDef& pat, int offsetX, int offsetY);
};
