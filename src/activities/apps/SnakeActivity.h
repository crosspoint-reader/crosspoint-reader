#pragma once

#include <cstdint>
#include <vector>

#include "activities/Activity.h"

class SnakeActivity final : public Activity {
 public:
  explicit SnakeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Snake", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return true; }

 private:
  enum State { PLAYING, GAME_OVER };

  State state = PLAYING;

  // Grid
  static constexpr int CELL_SIZE = 12;
  int gridW = 0;
  int gridH = 0;
  int offsetX = 0;
  int offsetY = 0;

  // Snake
  struct Point {
    int x, y;
  };
  std::vector<Point> snake;
  int dirX = 1, dirY = 0;     // current direction
  int nextDirX = 1, nextDirY = 0;  // buffered next direction

  // Food
  Point food;

  // Timing
  unsigned long lastStepMs = 0;
  static constexpr unsigned long STEP_INTERVAL_MS = 300;

  // Score
  int score = 0;

  void initGame();
  void step();
  void spawnFood();
  bool isSnakeAt(int x, int y) const;

  void renderPlaying() const;
  void renderGameOver() const;
};
