#include "ChessActivity.h"

#include <I18n.h>
#include <esp_random.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

// 50% gray — classic checkerboard for dark squares
static void fillDithered50(GfxRenderer& r, int x, int y, int w, int h) {
  for (int dy = 0; dy < h; dy++)
    for (int dx = (dy % 2); dx < w; dx += 2)
      r.drawPixel(x + dx, y + dy, true);
}

// 25% gray (light) for selected square
static void fillDithered25(GfxRenderer& r, int x, int y, int w, int h) {
  for (int dy = 0; dy < h; dy += 2)
    for (int dx = ((dy/2) % 2); dx < w; dx += 2)
      r.drawPixel(x + dx, y + dy, true);
}

// Draw chess piece icon at center (cx, cy) with given size
static void drawPieceIcon(GfxRenderer& r, int cx, int cy, int s, uint8_t piece, bool inverted, bool isBlackPiece) {
  bool fg = !inverted;
  int hs = s / 2;

  uint8_t type = piece;
  if (type >= 7) type -= 6; // B_PAWN=7 -> 1

  switch (type) {
    case 1: { // Pawn
      int hr = s / 4;
      for (int dy = -hr; dy <= hr; dy++) {
        int dx = 0;
        while ((dx+1)*(dx+1) + dy*dy <= hr*hr) dx++;
        if (isBlackPiece) {
          if (dx > 0) r.fillRect(cx - dx, cy - hs/2 + dy, dx*2+1, 1, fg);
        } else {
          if (dx > 0) { r.drawPixel(cx - dx, cy - hs/2 + dy, fg); r.drawPixel(cx + dx, cy - hs/2 + dy, fg); }
        }
      }
      for (int dy = 0; dy < hs/2; dy++) {
        int hw = s/6 + dy * s / (4 * hs);
        if (isBlackPiece) r.fillRect(cx - hw, cy - hs/2 + hr + dy, hw*2+1, 1, fg);
        else { r.drawPixel(cx - hw, cy - hs/2 + hr + dy, fg); r.drawPixel(cx + hw, cy - hs/2 + hr + dy, fg); }
      }
      int bw = s / 3;
      if (isBlackPiece) r.fillRect(cx - bw, cy + hs/3, bw*2+1, 3, fg);
      else r.drawRect(cx - bw, cy + hs/3, bw*2+1, 3, fg);
      break;
    }
    case 2: { // Rook
      int tw = s / 3;
      int cw = tw / 3;
      if (cw < 2) cw = 2;
      if (isBlackPiece) {
        r.fillRect(cx - tw, cy - hs + 1, cw, s/5, fg);
        r.fillRect(cx - cw/2, cy - hs + 1, cw, s/5, fg);
        r.fillRect(cx + tw - cw + 1, cy - hs + 1, cw, s/5, fg);
      } else {
        r.drawRect(cx - tw, cy - hs + 1, cw, s/5, fg);
        r.drawRect(cx - cw/2, cy - hs + 1, cw, s/5, fg);
        r.drawRect(cx + tw - cw + 1, cy - hs + 1, cw, s/5, fg);
      }
      if (isBlackPiece) r.fillRect(cx - tw, cy - hs + s/5 + 1, tw*2+1, s*2/5, fg);
      else r.drawRect(cx - tw, cy - hs + s/5 + 1, tw*2+1, s*2/5, fg);
      int bw = s * 2/5;
      if (isBlackPiece) r.fillRect(cx - bw, cy + hs/4, bw*2+1, 3, fg);
      else r.drawRect(cx - bw, cy + hs/4, bw*2+1, 3, fg);
      break;
    }
    case 3: { // Knight
      int w = s / 3;
      if (isBlackPiece) {
        r.fillRect(cx - w, cy - hs + 2, w + 2, s/4, fg);
        r.fillRect(cx - w - s/5, cy - hs + s/6, s/5, s/5, fg);
      } else {
        r.drawRect(cx - w, cy - hs + 2, w + 2, s/4, fg);
        r.drawRect(cx - w - s/5, cy - hs + s/6, s/5, s/5, fg);
      }
      if (isBlackPiece) r.fillRect(cx - w/2, cy - hs + s/4 + 2, w, s/3, fg);
      else r.drawRect(cx - w/2, cy - hs + s/4 + 2, w, s/3, fg);
      int bw = s * 2/5;
      if (isBlackPiece) r.fillRect(cx - bw, cy + hs/4, bw*2+1, 3, fg);
      else r.drawRect(cx - bw, cy + hs/4, bw*2+1, 3, fg);
      break;
    }
    case 4: { // Bishop
      if (isBlackPiece) r.fillRect(cx - 1, cy - hs + 1, 3, 3, fg);
      else r.drawRect(cx - 1, cy - hs + 1, 3, 3, fg);
      for (int dy = 0; dy < s * 2/5; dy++) {
        int hw = 1 + dy * s / (3 * s * 2/5);
        if (hw < 1) hw = 1;
        if (isBlackPiece) r.fillRect(cx - hw, cy - hs + 4 + dy, hw*2+1, 1, fg);
        else { r.drawPixel(cx - hw, cy - hs + 4 + dy, fg); r.drawPixel(cx + hw, cy - hs + 4 + dy, fg); }
      }
      r.fillRect(cx - s/4, cy - 1, s/2, 2, fg);
      int bw = s * 2/5;
      if (isBlackPiece) r.fillRect(cx - bw, cy + hs/4, bw*2+1, 3, fg);
      else r.drawRect(cx - bw, cy + hs/4, bw*2+1, 3, fg);
      break;
    }
    case 5: { // Queen
      int pw = s / 6;
      for (int p = -1; p <= 1; p++) {
        if (isBlackPiece) r.fillRect(cx + p * pw - 1, cy - hs + 1, 3, 4, fg);
        else r.drawRect(cx + p * pw - 1, cy - hs + 1, 3, 4, fg);
      }
      int bodyTop = cy - hs + 5;
      int bodyH = s * 2/5;
      for (int dy = 0; dy < bodyH; dy++) {
        int hw = s/5 + dy * s / (5 * bodyH);
        if (isBlackPiece) r.fillRect(cx - hw, bodyTop + dy, hw*2+1, 1, fg);
        else { r.drawPixel(cx - hw, bodyTop + dy, fg); r.drawPixel(cx + hw, bodyTop + dy, fg); }
      }
      int bw = s * 2/5;
      if (isBlackPiece) r.fillRect(cx - bw, cy + hs/4, bw*2+1, 3, fg);
      else r.drawRect(cx - bw, cy + hs/4, bw*2+1, 3, fg);
      break;
    }
    case 6: { // King
      r.fillRect(cx - 1, cy - hs, 3, s/4, fg);
      r.fillRect(cx - s/6, cy - hs + 2, s/3, 3, fg);
      int bodyTop = cy - hs + s/4;
      int bodyH = s * 2/5;
      for (int dy = 0; dy < bodyH; dy++) {
        int hw = s/5 + dy * s / (5 * bodyH);
        if (isBlackPiece) r.fillRect(cx - hw, bodyTop + dy, hw*2+1, 1, fg);
        else { r.drawPixel(cx - hw, bodyTop + dy, fg); r.drawPixel(cx + hw, bodyTop + dy, fg); }
      }
      int bw = s * 2/5;
      if (isBlackPiece) r.fillRect(cx - bw, cy + hs/4, bw*2+1, 3, fg);
      else r.drawRect(cx - bw, cy + hs/4, bw*2+1, 3, fg);
      break;
    }
    default: break;
  }
}

void ChessActivity::initBoard() {
  memset(board, EMPTY, sizeof(board));
  // Black pieces (top)
  board[0][0] = B_ROOK;   board[0][1] = B_KNIGHT; board[0][2] = B_BISHOP; board[0][3] = B_QUEEN;
  board[0][4] = B_KING;   board[0][5] = B_BISHOP; board[0][6] = B_KNIGHT; board[0][7] = B_ROOK;
  for (int c = 0; c < 8; c++) board[1][c] = B_PAWN;
  // White pieces (bottom)
  for (int c = 0; c < 8; c++) board[6][c] = W_PAWN;
  board[7][0] = W_ROOK;   board[7][1] = W_KNIGHT; board[7][2] = W_BISHOP; board[7][3] = W_QUEEN;
  board[7][4] = W_KING;   board[7][5] = W_BISHOP; board[7][6] = W_KNIGHT; board[7][7] = W_ROOK;
}

const char* ChessActivity::pieceChar(uint8_t piece) const {
  switch (piece) {
    case W_PAWN: case B_PAWN: return "P";
    case W_ROOK: case B_ROOK: return "R";
    case W_KNIGHT: case B_KNIGHT: return "N";
    case W_BISHOP: case B_BISHOP: return "B";
    case W_QUEEN: case B_QUEEN: return "Q";
    case W_KING: case B_KING: return "K";
    default: return "";
  }
}

void ChessActivity::addSlidingMoves(int fx, int fy, int dx, int dy,
                                     std::vector<std::pair<int, int>>& moves) const {
  int x = fx + dx, y = fy + dy;
  while (x >= 0 && x < 8 && y >= 0 && y < 8) {
    if (board[x][y] == EMPTY) {
      moves.push_back({x, y});
    } else {
      if (isEnemyPiece(board[x][y])) moves.push_back({x, y});
      break;
    }
    x += dx;
    y += dy;
  }
}

void ChessActivity::addMovesForPiece(int fx, int fy, std::vector<std::pair<int, int>>& moves) const {
  uint8_t p = board[fx][fy];
  int dir = isWhite(p) ? -1 : 1;

  switch (p) {
    case W_PAWN: case B_PAWN: {
      int startRow = isWhite(p) ? 6 : 1;
      // Forward
      if (fx + dir >= 0 && fx + dir < 8 && board[fx + dir][fy] == EMPTY) {
        moves.push_back({fx + dir, fy});
        // Double move from start
        if (fx == startRow && board[fx + 2 * dir][fy] == EMPTY) {
          moves.push_back({fx + 2 * dir, fy});
        }
      }
      // Captures
      for (int dc : {-1, 1}) {
        int nr = fx + dir, nc = fy + dc;
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8 && isEnemyPiece(board[nr][nc])) {
          moves.push_back({nr, nc});
        }
      }
      break;
    }
    case W_ROOK: case B_ROOK:
      for (auto [dx, dy] : {std::pair{1,0},{-1,0},{0,1},{0,-1}}) addSlidingMoves(fx, fy, dx, dy, moves);
      break;
    case W_BISHOP: case B_BISHOP:
      for (auto [dx, dy] : {std::pair{1,1},{1,-1},{-1,1},{-1,-1}}) addSlidingMoves(fx, fy, dx, dy, moves);
      break;
    case W_QUEEN: case B_QUEEN:
      for (auto [dx, dy] : {std::pair{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}})
        addSlidingMoves(fx, fy, dx, dy, moves);
      break;
    case W_KNIGHT: case B_KNIGHT:
      for (auto [dx, dy] : {std::pair{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}}) {
        int nr = fx + dx, nc = fy + dy;
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8 && !isOwnPiece(board[nr][nc])) {
          moves.push_back({nr, nc});
        }
      }
      break;
    case W_KING: case B_KING:
      for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
          if (dx == 0 && dy == 0) continue;
          int nr = fx + dx, nc = fy + dy;
          if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8 && !isOwnPiece(board[nr][nc])) {
            moves.push_back({nr, nc});
          }
        }
      }
      break;
    default: break;
  }
}

bool ChessActivity::isSquareAttacked(int tx, int ty, bool byWhite) {
  bool savedTurn = whiteTurn;
  whiteTurn = byWhite;
  std::vector<std::pair<int, int>> moves;
  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      uint8_t p = board[r][c];
      if (p == EMPTY) continue;
      if (byWhite ? !isWhite(p) : !isBlack(p)) continue;
      moves.clear();
      addMovesForPiece(r, c, moves);
      for (auto& [mr, mc] : moves) {
        if (mr == tx && mc == ty) { whiteTurn = savedTurn; return true; }
      }
    }
  }
  whiteTurn = savedTurn;
  return false;
}

bool ChessActivity::findKing(bool white, int& kx, int& ky) const {
  uint8_t target = white ? W_KING : B_KING;
  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      if (board[r][c] == target) { kx = r; ky = c; return true; }
    }
  }
  return false;
}

bool ChessActivity::wouldBeInCheck(int fx, int fy, int tx, int ty) {
  // Simulate move
  uint8_t savedTarget = board[tx][ty];
  uint8_t savedSource = board[fx][fy];
  board[tx][ty] = savedSource;
  board[fx][fy] = EMPTY;

  int kx, ky;
  findKing(whiteTurn, kx, ky);
  bool check = isSquareAttacked(kx, ky, !whiteTurn);

  // Undo
  board[fx][fy] = savedSource;
  board[tx][ty] = savedTarget;
  return check;
}

void ChessActivity::computeValidMoves(int fx, int fy) {
  validMoves.clear();
  std::vector<std::pair<int, int>> pseudo;
  addMovesForPiece(fx, fy, pseudo);
  for (auto& [tr, tc] : pseudo) {
    if (!wouldBeInCheck(fx, fy, tr, tc)) {
      validMoves.push_back({tr, tc});
    }
  }
}

bool ChessActivity::hasAnyLegalMove() {
  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      if (!isOwnPiece(board[r][c])) continue;
      std::vector<std::pair<int, int>> pseudo;
      addMovesForPiece(r, c, pseudo);
      for (auto& [tr, tc] : pseudo) {
        if (!wouldBeInCheck(r, c, tr, tc)) return true;
      }
    }
  }
  return false;
}

void ChessActivity::doMove(int fx, int fy, int tx, int ty) {
  uint8_t p = board[fx][fy];
  board[tx][ty] = p;
  board[fx][fy] = EMPTY;

  // Pawn promotion to queen
  if ((p == W_PAWN && tx == 0) || (p == B_PAWN && tx == 7)) {
    board[tx][ty] = isWhite(p) ? W_QUEEN : B_QUEEN;
  }
}

void ChessActivity::checkGameState() {
  int kx, ky;
  findKing(whiteTurn, kx, ky);
  inCheck = isSquareAttacked(kx, ky, !whiteTurn);

  if (!hasAnyLegalMove()) {
    gameOver = true;
    state = GAME_OVER;
    gameOverMsg = inCheck ? tr(STR_CHECKMATE) : tr(STR_STALEMATE);
  }
}

void ChessActivity::botMove() {
  struct Move { int fx, fy, tx, ty; };
  static constexpr int MAX_MOVES = 218;
  Move legalMoves[MAX_MOVES];
  int moveCount = 0;

  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      if (!isOwnPiece(board[r][c])) continue;
      std::vector<std::pair<int,int>> targets;
      addMovesForPiece(r, c, targets);
      for (auto& [tr, tc] : targets) {
        if (!wouldBeInCheck(r, c, tr, tc) && moveCount < MAX_MOVES) {
          legalMoves[moveCount++] = {c, r, tc, tr};
        }
      }
    }
  }
  if (moveCount == 0) return;
  auto& move = legalMoves[esp_random() % moveCount];
  doMove(move.fy, move.fx, move.ty, move.tx);
  whiteTurn = !whiteTurn;
  checkGameState();
}

void ChessActivity::onEnter() {
  Activity::onEnter();
  state = SETUP;
  setupIndex = 0;
  vsBot = false;
  botThinking = false;
  requestUpdate();
}

void ChessActivity::loop() {
  if (state == SETUP) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      setupIndex = 1 - setupIndex;
      requestUpdate();
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      vsBot = (setupIndex == 1);
      initBoard();
      state = SELECT_PIECE;
      whiteTurn = true;
      inCheck = false;
      gameOver = false;
      cursorX = 4;
      cursorY = 7;
      validMoves.clear();
      requestUpdate();
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  // Bot thinking timer
  if (botThinking) {
    if (millis() - botThinkStart > 600) {
      botThinking = false;
      botMove();
      requestUpdate();
    }
    return;
  }

  if (state == GAME_OVER) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state = SETUP;
      setupIndex = 0;
      vsBot = false;
      botThinking = false;
      requestUpdate();
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  bool moved = false;
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) { if (cursorY > 0) cursorY--; moved = true; }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) { if (cursorY < 7) cursorY++; moved = true; }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) { if (cursorX > 0) cursorX--; moved = true; }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) { if (cursorX < 7) cursorX++; moved = true; }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (state == SELECT_PIECE) {
      if (isOwnPiece(board[cursorY][cursorX])) {
        selectedX = cursorX;
        selectedY = cursorY;
        computeValidMoves(cursorY, cursorX);
        if (!validMoves.empty()) {
          state = SELECT_TARGET;
        }
        moved = true;
      }
    } else if (state == SELECT_TARGET) {
      // Check if target is valid
      auto it = std::find(validMoves.begin(), validMoves.end(), std::pair<int, int>{cursorY, cursorX});
      if (it != validMoves.end()) {
        doMove(selectedY, selectedX, cursorY, cursorX);
        whiteTurn = !whiteTurn;
        state = SELECT_PIECE;
        validMoves.clear();
        checkGameState();
        // Trigger bot move
        if (vsBot && !gameOver) {
          botThinking = true;
          botThinkStart = millis();
          requestUpdate();
        }
      } else if (isOwnPiece(board[cursorY][cursorX])) {
        // Re-select different piece
        selectedX = cursorX;
        selectedY = cursorY;
        computeValidMoves(cursorY, cursorX);
        if (validMoves.empty()) {
          state = SELECT_PIECE;
        }
      } else {
        state = SELECT_PIECE;
        validMoves.clear();
      }
      moved = true;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (state == SELECT_TARGET) {
      state = SELECT_PIECE;
      validMoves.clear();
      moved = true;
    } else {
      finish();
      return;
    }
  }

  if (moved) requestUpdate();
}

void ChessActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  if (state == SETUP) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CHESS));
    int y = pageHeight / 2 - 60;
    renderer.drawCenteredText(UI_10_FONT_ID, y, "Select Mode:");
    y += 40;
    if (setupIndex == 0) {
      renderer.fillRect(pageWidth/2 - 100, y - 4, 200, 30, true);
      renderer.drawCenteredText(UI_12_FONT_ID, y, "vs Human", false, EpdFontFamily::BOLD);
    } else {
      renderer.drawCenteredText(UI_12_FONT_ID, y, "vs Human");
    }
    y += 45;
    if (setupIndex == 1) {
      renderer.fillRect(pageWidth/2 - 100, y - 4, 200, 30, true);
      renderer.drawCenteredText(UI_12_FONT_ID, y, "vs Bot (Random)", false, EpdFontFamily::BOLD);
    } else {
      renderer.drawCenteredText(UI_12_FONT_ID, y, "vs Bot (Random)");
    }
    const auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_SELECT), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // Compact header
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CHESS));

  const int headerBottom = metrics.topPadding + metrics.headerHeight + 4;
  const int hintsTop = pageHeight - metrics.buttonHintsHeight;

  // Reserve 40px below board for turn info
  const int infoHeight = 40;
  const int availHeight = hintsTop - headerBottom - infoHeight - 8;
  const int availWidth = pageWidth - 2 * 15; // 15px padding each side

  int cellSize = std::min(availWidth / 8, availHeight / 8);

  const int gridSize = cellSize * 8;
  const int coordSpace = 14; // space for coordinate labels
  const int gridX = (pageWidth - gridSize - coordSpace) / 2;
  const int gridY = headerBottom + (availHeight - gridSize) / 2;
  const int pieceSize = cellSize * 2 / 3;

  // Double-line board border
  renderer.drawRect(gridX - 1, gridY - 1, gridSize + 2, gridSize + 2, true);
  renderer.drawRect(gridX - 3, gridY - 3, gridSize + 6, gridSize + 6, true);

  // Draw board
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      int px = gridX + col * cellSize;
      int py = gridY + row * cellSize;
      bool darkSquare = ((row + col) % 2 == 1);
      bool isCursor = (col == cursorX && row == cursorY);
      bool isSelected = (state == SELECT_TARGET && col == selectedX && row == selectedY);
      bool isValidTarget = false;

      if (state == SELECT_TARGET) {
        for (auto& [mr, mc] : validMoves) {
          if (mr == row && mc == col) { isValidTarget = true; break; }
        }
      }

      // Background
      renderer.fillRect(px, py, cellSize, cellSize, false); // white base
      if (darkSquare) {
        fillDithered50(renderer, px, py, cellSize, cellSize);
      }
      if (isSelected) {
        fillDithered25(renderer, px, py, cellSize, cellSize);
      }

      // Cursor: 3px thick border
      if (isCursor) {
        renderer.drawRect(px, py, cellSize, cellSize, true);
        renderer.drawRect(px + 1, py + 1, cellSize - 2, cellSize - 2, true);
        renderer.drawRect(px + 2, py + 2, cellSize - 4, cellSize - 4, true);
      }

      // Valid move indicator: small filled circle
      if (isValidTarget && !isCursor) {
        int mcx = px + cellSize / 2;
        int mcy = py + cellSize / 2;
        int mr = cellSize / 5;
        for (int dy = -mr; dy <= mr; dy++) {
          int dx = 0;
          while ((dx+1)*(dx+1) + dy*dy <= mr*mr) dx++;
          if (dx > 0) renderer.fillRect(mcx - dx, mcy + dy, dx*2+1, 1, true);
          else renderer.drawPixel(mcx, mcy + dy, true);
        }
      }

      // Piece icon
      uint8_t piece = board[row][col];
      if (piece != EMPTY) {
        int pcx = px + cellSize / 2;
        int pcy = py + cellSize / 2;
        bool blackPiece = isBlack(piece);
        drawPieceIcon(renderer, pcx, pcy, pieceSize, piece, false, blackPiece);
      }

      // Cell border
      renderer.drawRect(px, py, cellSize, cellSize, true);
    }
  }

  // Coordinate labels
  static const char* const colLabels[] = {"a","b","c","d","e","f","g","h"};
  for (int c = 0; c < 8; c++) {
    int lx = gridX + c * cellSize + cellSize / 2 - 3;
    renderer.drawText(SMALL_FONT_ID, lx, gridY + gridSize + 3, colLabels[c]);
  }
  for (int row = 0; row < 8; row++) {
    char rowLabel[2] = {(char)('8' - row), 0};
    renderer.drawText(SMALL_FONT_ID, gridX + gridSize + 5, gridY + row * cellSize + cellSize / 2 - 5, rowLabel);
  }

  // Turn/check info below board
  int infoY = gridY + gridSize + coordSpace + 4;
  std::string turnInfo = whiteTurn ? tr(STR_WHITE) : tr(STR_BLACK_PIECE);
  if (botThinking) {
    turnInfo += " - Thinking...";
  } else if (inCheck) {
    turnInfo += std::string(" - ") + tr(STR_CHECK);
  }
  if (vsBot) turnInfo += "  [Bot]";
  renderer.drawCenteredText(UI_10_FONT_ID, infoY, turnInfo.c_str(), true, EpdFontFamily::BOLD);

  if (state == GAME_OVER) {
    GUI.drawPopup(renderer, gameOverMsg.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_NEW_GAME), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    const auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_SELECT), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
