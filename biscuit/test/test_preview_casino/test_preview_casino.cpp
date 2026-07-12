// test/test_preview_casino/test_preview_casino.cpp
// Preview all casino screens — run: pio test -e native -f test_preview_casino

#include <unity.h>
#include "BitmapRenderer.h"

static GfxRenderer renderer;

#define UI_10_FONT_ID 10
#define UI_12_FONT_ID 12
#define SMALL_FONT_ID 8

static void drawHeader(const char* title) {
  renderer.drawCenteredText(UI_12_FONT_ID, 12, title, true, 1);
  renderer.drawLine(15, 42, 465, 42);
}

static void drawCredits(int credits) {
  char buf[32];
  snprintf(buf, sizeof(buf), "Credits: %d", credits);
  renderer.drawText(UI_10_FONT_ID, 15, 50, buf, true, 1);
  renderer.drawLine(0, 72, 480, 72);
}

static void drawHints(const char* a, const char* b, const char* c, const char* d) {
  renderer.drawLine(0, 768, 480, 768);
  if (a) renderer.drawText(SMALL_FONT_ID, 15, 775, a);
  if (d) {
    int w = renderer.getTextWidth(SMALL_FONT_ID, d);
    renderer.drawText(SMALL_FONT_ID, 465 - w, 775, d);
  }
}

static void drawListItem(int y, const char* text, bool sel, const char* sub = nullptr) {
  if (sel) {
    renderer.fillRect(0, y, 480, sub ? 50 : 36, true);
    renderer.drawText(UI_10_FONT_ID, 20, y + 6, text, false);
    if (sub) renderer.drawText(SMALL_FONT_ID, 20, y + 28, sub, false);
  } else {
    renderer.drawText(UI_10_FONT_ID, 20, y + 6, text);
    if (sub) renderer.drawText(SMALL_FONT_ID, 20, y + 28, sub);
  }
}

static void drawCard(int x, int y, const char* rank, const char* suit, bool faceDown = false) {
  int cw = 55, ch = 75;
  renderer.fillRect(x + 2, y + 2, cw, ch, true);
  renderer.fillRect(x, y, cw, ch, false);
  renderer.drawRect(x, y, cw, ch);
  renderer.drawRect(x + 1, y + 1, cw - 2, ch - 2);
  if (faceDown) {
    for (int i = 6; i < cw - 6; i += 4)
      renderer.drawLine(x + i, y + 6, x + i, y + ch - 6);
    for (int j = 6; j < ch - 6; j += 4)
      renderer.drawLine(x + 6, y + j, x + cw - 6, y + j);
    return;
  }
  renderer.drawText(UI_10_FONT_ID, x + 5, y + 4, rank);
  int sw = renderer.getTextWidth(UI_10_FONT_ID, suit);
  renderer.drawText(UI_10_FONT_ID, x + (cw - sw) / 2, y + 30, suit);
  int rw = renderer.getTextWidth(UI_10_FONT_ID, rank);
  renderer.drawText(UI_10_FONT_ID, x + cw - rw - 5, y + ch - 22, rank);
}

static void drawSlotReel(int x, int y, const char* sym) {
  int rw = 110, rh = 120;
  renderer.fillRect(x + 3, y + 3, rw, rh, true);
  renderer.fillRect(x, y, rw, rh, false);
  renderer.drawRect(x, y, rw, rh);
  renderer.drawRect(x + 2, y + 2, rw - 4, rh - 4);
  int tw = renderer.getTextWidth(UI_12_FONT_ID, sym, 1);
  renderer.drawText(UI_12_FONT_ID, x + (rw - tw) / 2, y + rh / 2 - 12, sym, true, 1);
}

static void drawResultOverlay(const char* msg, const char* amount, int credits) {
  renderer.fillRect(30, 280, 420, 180, false);
  renderer.drawRect(30, 280, 420, 180);
  renderer.drawRect(32, 282, 416, 176);
  renderer.drawCenteredText(UI_12_FONT_ID, 305, msg, true, 1);
  renderer.drawCenteredText(UI_10_FONT_ID, 350, amount);
  char buf[32]; snprintf(buf, sizeof(buf), "Balance: %d", credits);
  renderer.drawCenteredText(UI_10_FONT_ID, 390, buf);
  renderer.drawCenteredText(SMALL_FONT_ID, 425, "[OK] Continue");
}

// ========== SCREENS ==========

void preview_lobby() {
  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 12, "biscuit. Casino", true, 1);
  renderer.drawLine(15, 42, 465, 42);
  drawCredits(2450);

  int y = 90;
  drawListItem(y, "Slot Machine", false, "Spin 3 reels, match symbols"); y += 52;
  drawListItem(y, "Blackjack", true, "Beat the dealer to 21"); y += 52;
  drawListItem(y, "Coin Flip", false, "Pick heads or tails, 2x payout"); y += 52;
  drawListItem(y, "Higher / Lower", false, "Guess the next card, streak bonus");

  drawHints("Exit", "Play", "Up", "Down");
  TEST_ASSERT_TRUE(renderer.saveBMP("test/preview_casino_lobby.bmp"));
}

void preview_slots() {
  renderer.clearScreen();
  drawHeader("Slot Machine");
  drawCredits(2450);

  int startX = 35;
  drawSlotReel(startX, 140, "7");
  drawSlotReel(startX + 130, 140, "7");
  drawSlotReel(startX + 260, 140, "CHR");

  renderer.drawCenteredText(SMALL_FONT_ID, 300, "--- PAYOUTS ---");
  renderer.drawCenteredText(SMALL_FONT_ID, 322, "3x 7 = 50x  |  3x BAR = 20x");
  renderer.drawCenteredText(SMALL_FONT_ID, 340, "3x CHR = 10x  |  2x any = 2x");
  renderer.drawCenteredText(UI_10_FONT_ID, 390, "< Bet: 25 >");
  drawHints("Back", "Spin", "Bet-", "Bet+");
  TEST_ASSERT_TRUE(renderer.saveBMP("test/preview_casino_slots.bmp"));
}

void preview_slots_jackpot() {
  renderer.clearScreen();
  drawHeader("Slot Machine");
  drawCredits(3700);

  int startX = 35;
  drawSlotReel(startX, 140, "7");
  drawSlotReel(startX + 130, 140, "7");
  drawSlotReel(startX + 260, 140, "7");

  drawResultOverlay("JACKPOT!", "+1250 credits", 3700);
  TEST_ASSERT_TRUE(renderer.saveBMP("test/preview_casino_jackpot.bmp"));
}

void preview_blackjack() {
  renderer.clearScreen();
  drawHeader("Blackjack");
  drawCredits(2400);

  renderer.drawText(UI_10_FONT_ID, 15, 90, "Dealer:", true, 1);
  drawCard(15, 115, "K", "S");
  drawCard(77, 115, "", "", true);  // face down
  renderer.drawText(UI_10_FONT_ID, 155, 140, "= 10 + ?");

  renderer.drawText(UI_10_FONT_ID, 15, 210, "You:", true, 1);
  drawCard(15, 235, "A", "H");
  drawCard(77, 235, "8", "D");
  renderer.drawText(UI_10_FONT_ID, 155, 260, "= 19", true, 1);

  drawHints("Stand", "Hit", "", "Stand");
  TEST_ASSERT_TRUE(renderer.saveBMP("test/preview_casino_blackjack.bmp"));
}

void preview_blackjack_win() {
  renderer.clearScreen();
  drawHeader("Blackjack");
  drawCredits(2500);

  renderer.drawText(UI_10_FONT_ID, 15, 90, "Dealer:", true, 1);
  drawCard(15, 115, "K", "S");
  drawCard(77, 115, "7", "C");
  drawCard(139, 115, "Q", "D");
  renderer.drawText(UI_10_FONT_ID, 215, 140, "= 27 BUST");

  renderer.drawText(UI_10_FONT_ID, 15, 210, "You:", true, 1);
  drawCard(15, 235, "A", "H");
  drawCard(77, 235, "8", "D");
  renderer.drawText(UI_10_FONT_ID, 155, 260, "= 19", true, 1);

  drawResultOverlay("Dealer Busts!", "+50 credits", 2500);
  TEST_ASSERT_TRUE(renderer.saveBMP("test/preview_casino_bj_win.bmp"));
}

void preview_coinflip() {
  renderer.clearScreen();
  drawHeader("Coin Flip");
  drawCredits(2450);

  renderer.drawCenteredText(UI_10_FONT_ID, 190, "Pick your side:");

  int cx = 240, cy = 280, cr = 70;
  renderer.drawRect(cx - cr, cy - cr, cr * 2, cr * 2);
  renderer.drawCenteredText(UI_12_FONT_ID, cy - 12, "HEADS", true, 1);

  renderer.drawCenteredText(UI_10_FONT_ID, 400, "> HEADS <");
  renderer.drawCenteredText(UI_10_FONT_ID, 440, "  TAILS  ");
  renderer.drawCenteredText(UI_10_FONT_ID, 500, "Bet: 25");

  drawHints("Cancel", "Flip!", "Heads", "Tails");
  TEST_ASSERT_TRUE(renderer.saveBMP("test/preview_casino_coinflip.bmp"));
}

void preview_highlow() {
  renderer.clearScreen();
  drawHeader("Higher / Lower");
  drawCredits(2380);

  drawCard(200, 130, "7", "H");

  renderer.drawCenteredText(UI_10_FONT_ID, 240, "Streak: 3");
  renderer.drawCenteredText(UI_12_FONT_ID, 280, "Pot: 84", true, 1);
  renderer.drawCenteredText(UI_10_FONT_ID, 350, "Higher or Lower?");

  drawHints("", "Cash Out", "Higher", "Lower");
  TEST_ASSERT_TRUE(renderer.saveBMP("test/preview_casino_highlow.bmp"));
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(preview_lobby);
  RUN_TEST(preview_slots);
  RUN_TEST(preview_slots_jackpot);
  RUN_TEST(preview_blackjack);
  RUN_TEST(preview_blackjack_win);
  RUN_TEST(preview_coinflip);
  RUN_TEST(preview_highlow);
  return UNITY_END();
}
