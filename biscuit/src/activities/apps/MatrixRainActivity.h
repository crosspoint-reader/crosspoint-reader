#pragma once
#include <cstdint>
#include "activities/Activity.h"

class MatrixRainActivity final : public Activity {
 public:
  explicit MatrixRainActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("MatrixRain", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return true; }

 private:
  int cols = 0;
  int rows = 0;
  static constexpr int CHAR_W = 10;
  static constexpr int CHAR_H = 14;

  static constexpr int MAX_COLS = 48;  // 480 / 10
  static constexpr int MAX_ROWS = 57;  // 800 / 14

  int dropHead[MAX_COLS] = {};
  int dropLength[MAX_COLS] = {};
  int dropSpeed[MAX_COLS] = {};
  bool dropActive[MAX_COLS] = {};

  char grid[MAX_ROWS * MAX_COLS] = {};

  unsigned long lastFrameMs = 0;
  static constexpr unsigned long FRAME_INTERVAL_MS = 800;
  int frameCount = 0;

  int speedLevel = 1;  // 0=slow (1200ms), 1=normal (800ms), 2=fast (500ms)
  static constexpr unsigned long SPEED_INTERVALS[] = {1200, 800, 500};

  int density = 2;  // 0=sparse, 1=normal, 2=dense

  void initColumns();
  void advanceFrame();
  void spawnDrop(int col);
  char randomChar();
  void drawChar(int col, int row, char c, int intensity);
};
