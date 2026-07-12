#pragma once
#include <cstdint>
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class MazeActivity final : public Activity {
 public:
  explicit MazeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Maze", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == SOLVING; }

 private:
  enum State { SIZE_SELECT, PLAYING, SOLVED_MSG, SOLVING, SOLVE_DONE };
  State state = SIZE_SELECT;
  ButtonNavigator buttonNavigator;

  static constexpr int MAX_W = 40;
  static constexpr int MAX_H = 60;
  uint8_t maze[MAX_H][MAX_W] = {};
  int mazeW = 20;
  int mazeH = 30;
  int cellSize = 0;
  int offsetX = 0, offsetY = 0;

  int playerX = 0, playerY = 0;
  int exitX = 0, exitY = 0;
  int moveCount = 0;
  unsigned long startTime = 0;

  int sizeIndex = 1;
  static constexpr int SIZES_W[] = {10, 20, 40};
  static constexpr int SIZES_H[] = {15, 30, 60};

  // BFS solver state
  uint8_t visited[MAX_H * MAX_W / 8 + 1] = {};
  int16_t parentIdx[MAX_H * MAX_W] = {};
  int16_t solveQueue[MAX_H * MAX_W] = {};
  int solveQueueHead = 0, solveQueueTail = 0;
  unsigned long lastSolveStepMs = 0;
  static constexpr unsigned long SOLVE_STEP_MS = 50;
  int solveBatchSize = 5;
  bool solvePathFound = false;

  static constexpr int MAX_PATH = 2400;
  int16_t solvePath[MAX_PATH] = {};
  int solvePathLen = 0;
  int solvePathDrawn = 0;
  enum SolvePhase { PHASE_BFS, PHASE_TRACE_PATH };
  SolvePhase solvePhase = PHASE_BFS;

  bool isVisited(int x, int y) const;
  void setVisited(int x, int y);

  void generateMaze();
  void startSolving();
  void solveStep();
  void tracePath();

  void drawMaze();
  void drawPlayer();
  void drawExit();
  void calculateLayout();

  static void fillDithered50(GfxRenderer& r, int x, int y, int w, int h);
};
