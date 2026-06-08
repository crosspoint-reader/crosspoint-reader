#pragma once
#include <cstdint>

#include "activities/Activity.h"

class SudokuActivity final : public Activity {
 public:
  enum class Difficulty : uint8_t { EASY = 0, MEDIUM = 1, HARD = 2, EXPERT = 3 };

 private:
  enum class State : uint8_t { SELECT_DIFFICULTY, PLAY, SELECT_NUMBER, COMPLETE };

  struct Move {
    uint8_t row, col, oldVal, newVal;
  };

  static constexpr int UNDO_LIMIT = 50;
  static constexpr int GRID = 9;

  State state = State::SELECT_DIFFICULTY;
  Difficulty difficulty = Difficulty::EASY;

  uint8_t board[GRID][GRID] = {};
  bool given[GRID][GRID] = {};
  bool error[GRID][GRID] = {};

  int cursorRow = 0;
  int cursorCol = 0;
  int selectedNumber = 1;

  Move undoStack[UNDO_LIMIT];
  int undoCount = 0;

  uint32_t startTime = 0;
  uint32_t pausedElapsed = 0;
  bool longPressFired = false;

  void generatePuzzle();
  void applyPermutation(uint8_t grid[GRID][GRID]);
  void removeCells(uint8_t grid[GRID][GRID], int clues);
  void checkErrors();
  bool isSolved() const;
  void placeNumber(uint8_t row, uint8_t col, uint8_t num);
  void undoMove();
  void saveGame() const;
  bool loadGame();

  void renderSelectDifficulty();
  void renderPlay();
  void renderComplete();
  void renderGrid(int gridX, int gridY, int cellW, int cellH, int thinLine, int thickLine);

  uint32_t elapsedSeconds() const;
  static const char* difficultyName(Difficulty d);

 public:
  SudokuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
