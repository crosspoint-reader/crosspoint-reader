#pragma once

#include <cstdint>

#include "activities/Activity.h"

class TetrisActivity final : public Activity {
 public:
  explicit TetrisActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Tetris", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return true; }

 private:
  enum State { PLAYING, GAME_OVER };

  State state = PLAYING;

  // Board
  static constexpr int BOARD_W = 10;
  static constexpr int BOARD_H = 20;
  int cellSize = 20;
  uint8_t board[BOARD_H][BOARD_W] = {};  // 0 = empty, 1 = filled

  // Piece rendering offset
  int boardOffsetX = 0;
  int boardOffsetY = 0;

  // Tetrominoes: 7 pieces, 4 rotations each, stored as 4x4 bit patterns
  struct Piece {
    uint16_t shape[4];  // 4 rotations, each a 4x4 bitfield (row-major, MSB=top-left)
    // Bits: row0[3:0] row1[3:0] row2[3:0] row3[3:0]
  };

  static const Piece PIECES[7];

  // Current piece
  int currentPiece = 0;
  int currentRotation = 0;
  int pieceX = 0;
  int pieceY = 0;

  // Next piece
  int nextPiece = 0;

  // Score / level
  int score = 0;
  int linesCleared = 0;
  int level = 1;

  // Timing
  unsigned long lastDropMs = 0;
  unsigned long getDropInterval() const;

  // Game logic
  void initGame();
  void spawnPiece();
  bool canPlace(int piece, int rotation, int x, int y) const;
  void lockPiece();
  int clearLines();
  void step();

  // Piece bit access
  static bool getPieceBit(uint16_t shape, int row, int col);

  void renderPlaying() const;
  void renderGameOver() const;
  void drawCell(int screenX, int screenY, bool filled) const;

  static int randomPiece();
};
