#include "CasinoActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_random.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

// Static constexpr initializers
constexpr int CasinoActivity::BET_OPTIONS[];
constexpr const char* CasinoActivity::SUIT_CHARS[];
constexpr const char* CasinoActivity::RANK_CHARS[];

// Symbol IDs:
//  0=Seven, 1=BAR, 2=Cherry, 3=Bell, 4=Star, 5=Diamond,
//  6=Lemon, 7=Orange, 8=Grape, 9=Melon, 10=Plum, 11=SevenOutline(wild)
const CasinoActivity::SlotMachineType CasinoActivity::MACHINES[NUM_MACHINES] = {
    {"Classic",        "Traditional 3-reel slots",    6, {{0,1,2,3,4,5,0,0},   {50,20,10,8,5,3,0,0}},    2, 0xFF, false, false, 10},
    {"Fruit Frenzy",   "Fruity fun, free spins!",     6, {{2,6,7,8,9,10,0,0},  {15,12,10,8,6,4,0,0}},    2, 0xFF, true,  false, 10},
    {"Lucky 7s",       "Wild sevens match anything",  5, {{0,11,1,3,4,0,0,0},  {50,25,20,8,5,0,0,0}},    2, 1,    false, false, 25},
    {"Diamond Deluxe", "Hold reels, big diamonds",    6, {{5,4,3,1,2,0,0,0},   {40,15,10,8,5,3,0,0}},    3, 0xFF, false, true,  50},
    {"High Roller",    "Big bets, bigger payouts",    4, {{0,1,5,4,0,0,0,0},   {80,30,20,10,0,0,0,0}},   3, 0xFF, false, false, 100},
};

bool CasinoActivity::isRed(int n) {
  static constexpr int reds[] = {1,3,5,7,9,12,14,16,18,19,21,23,25,27,30,32,34,36};
  for (int r : reds) { if (r == n) return true; }
  return false;
}

// ================================================================
// LIFECYCLE
// ================================================================

void CasinoActivity::onEnter() {
  Activity::onEnter();
  screen = LOBBY;
  lobbyIndex = 0;
  betIndex = 1;
  showingResult = false;
  loadCredits();
  bjShuffle();
  // Loot box init
  lbScreen = LB_MAIN_MENU;
  lbMenuIndex = 0;
  lbAnimState = LB_ANIM_IDLE;
  lbLoadCollection();
  requestUpdate();
}

void CasinoActivity::onExit() {
  Activity::onExit();
  saveCredits();
  lbSaveCollection();
}

// ================================================================
// CREDITS — persistent on SD
// ================================================================

void CasinoActivity::loadCredits() {
  Storage.mkdir("/biscuit");
  auto file = Storage.open(SAVE_PATH);
  if (file && !file.isDirectory()) {
    uint8_t buf[4];
    if (file.read(buf, 4) == 4) {
      credits = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
      LOG_DBG("Casino", "Loaded credits: %d", credits);
    }
    file.close();
  }
  if (credits <= 0) credits = 1000;  // free refill when broke
}

void CasinoActivity::saveCredits() {
  auto file = Storage.open(SAVE_PATH, O_WRITE | O_CREAT | O_TRUNC);
  if (file) {
    uint8_t buf[4] = {
        (uint8_t)(credits & 0xFF),
        (uint8_t)((credits >> 8) & 0xFF),
        (uint8_t)((credits >> 16) & 0xFF),
        (uint8_t)((credits >> 24) & 0xFF),
    };
    file.write(buf, 4);
    file.close();
  }
}

// ================================================================
// MAIN LOOP — dispatches to active game
// ================================================================

void CasinoActivity::loop() {
  // Dismiss result overlay
  if (showingResult) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
        mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      showingResult = false;
      // Refill if broke
      if (credits <= 0) { credits = 1000; saveCredits(); }
      requestUpdate();
      return;
    }
    return;
  }

  switch (screen) {
    case LOBBY: {
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) { finish(); return; }
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        switch (lobbyIndex) {
          case 0:
            screen = SLOTS;
            slotsScreen = SLOTS_MENU;
            slotsMachineMenuIndex = 0;
            break;
          case 1: screen = BLACKJACK; bjState = BJ_BET; break;
          case 2: screen = COINFLIP; coinState = COIN_BET; break;
          case 3: screen = HIGHLOW; hlState = HL_BET; hlStreak = 0; break;
          case 4: screen = ROULETTE; rlState = RL_BET; break;
          case 5:
            screen = LOOTBOX;
            lbScreen = LB_MAIN_MENU;
            lbMenuIndex = 0;
            lbAnimState = LB_ANIM_IDLE;
            break;
          case 6:
            resetConfirmCount++;
            if (resetConfirmCount >= 3) {
              resetCredits();
              resetConfirmCount = 0;
            }
            requestUpdate();
            return;
        }
        requestUpdate(); return;
      }
      buttonNavigator.onNext([this] { lobbyIndex = ButtonNavigator::nextIndex(lobbyIndex, LOBBY_COUNT); resetConfirmCount = 0; requestUpdate(); });
      buttonNavigator.onPrevious([this] { lobbyIndex = ButtonNavigator::previousIndex(lobbyIndex, LOBBY_COUNT); resetConfirmCount = 0; requestUpdate(); });
      break;
    }
    case SLOTS: slotsLoop(); break;
    case BLACKJACK: bjLoop(); break;
    case COINFLIP: coinLoop(); break;
    case HIGHLOW: hlLoop(); break;
    case ROULETTE: rlLoop(); break;
    case LOOTBOX: lbLoop(); break;
  }
}

// ================================================================
// RENDER — dispatches to active game
// ================================================================

void CasinoActivity::render(RenderLock&&) {
  renderer.clearScreen();

  switch (screen) {
    case LOBBY: renderLobby(); break;
    case SLOTS: slotsRender(); break;
    case BLACKJACK: bjRender(); break;
    case COINFLIP: coinRender(); break;
    case HIGHLOW: hlRender(); break;
    case ROULETTE: rlRender(); break;
    case LOOTBOX: lbRender(); return;  // lbRender calls displayBuffer itself
  }

  // Result overlay
  if (showingResult) {
    // Dim background with border box
    int boxY = 300, boxH = 180;
    renderer.fillRect(20, boxY, 440, boxH, false);
    renderer.drawRect(20, boxY, 440, boxH);
    renderer.drawRect(22, boxY + 2, 436, boxH - 4);

    renderer.drawCenteredText(UI_12_FONT_ID, boxY + 25, resultMessage.c_str(), true, EpdFontFamily::BOLD);

    char amtBuf[32];
    if (resultAmount > 0) {
      snprintf(amtBuf, sizeof(amtBuf), "+%d credits", resultAmount);
    } else if (resultAmount < 0) {
      snprintf(amtBuf, sizeof(amtBuf), "%d credits", resultAmount);
    } else {
      snprintf(amtBuf, sizeof(amtBuf), "Push");
    }
    renderer.drawCenteredText(UI_10_FONT_ID, boxY + 70, amtBuf);

    char balBuf[32];
    snprintf(balBuf, sizeof(balBuf), "Balance: %d", credits);
    renderer.drawCenteredText(UI_10_FONT_ID, boxY + 110, balBuf);

    renderer.drawCenteredText(SMALL_FONT_ID, boxY + 145, "[OK] Continue");
  }

  renderer.displayBuffer();
}

// ================================================================
// RENDERING HELPERS
// ================================================================

void CasinoActivity::renderCreditsBar() {
  const auto pageWidth = renderer.getScreenWidth();
  char buf[32];
  snprintf(buf, sizeof(buf), "Credits: %d", credits);
  renderer.drawText(UI_10_FONT_ID, 15, 50, buf, true, EpdFontFamily::BOLD);
  renderer.drawLine(0, 72, pageWidth, 72);
}

void CasinoActivity::renderBetSelector(int y) {
  char buf[32];
  snprintf(buf, sizeof(buf), "< Bet: %d >", currentBet());
  renderer.drawCenteredText(UI_10_FONT_ID, y, buf);
}

void CasinoActivity::renderLobby() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.drawCenteredText(UI_12_FONT_ID, 12, "biscuit. Casino", true, EpdFontFamily::BOLD);
  renderer.drawLine(15, 42, pageWidth - 15, 42);

  renderCreditsBar();

  int contentTop = 90;
  int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  static const char* games[] = {"Slot Machine", "Blackjack", "Coin Flip", "Higher / Lower", "Roulette", "Loot Box", "Reset Credits"};
  static const char* descs[] = {"5 machines with powerups", "Beat the dealer to 21", "Pick heads or tails, 2x payout", "Guess the next card, streak bonus", "European roulette, 0-36", "Gacha pulls & collection", "Start fresh with 1000 credits"};

  static const char* resetDescs[] = {"Start fresh with 1000 credits", "Are you sure? Press again.", "Really sure? Press once more."};
  const char* resetDesc = resetDescs[resetConfirmCount < 3 ? resetConfirmCount : 2];

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, LOBBY_COUNT, lobbyIndex,
      [](int i) { return std::string(games[i]); },
      [resetDesc](int i) { return std::string(i == 6 ? resetDesc : descs[i]); });

  const auto labels = mappedInput.mapLabels("Exit", "Play", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// Draw a card suit shape using only primitives
// suit: 0=Spades, 1=Hearts, 2=Diamonds, 3=Clubs
static void drawSuitShape(GfxRenderer& r, int cx, int cy, int sz, int suit) {
  switch (suit) {
    case 0: {  // Spades: upward triangle + two bumps + stem
      // Upward pointing body
      for (int dy = -sz; dy <= sz / 2; dy++) {
        int halfW = (dy + sz) * sz / (sz + sz / 2);
        if (halfW < 0) halfW = 0;
        if (halfW > 0) r.fillRect(cx - halfW, cy + dy, halfW * 2 + 1, 1, true);
      }
      // Two small bumps at bottom sides
      int bumpR = sz / 3;
      if (bumpR < 1) bumpR = 1;
      for (int dy = -bumpR; dy <= bumpR; dy++) {
        int dx = bumpR - (dy < 0 ? -dy : dy);
        if (dx > 0) {
          r.fillRect(cx - sz / 2 - dx / 2, cy + sz / 2 + dy, dx, 1, true);
          r.fillRect(cx + sz / 2 - dx / 2, cy + sz / 2 + dy, dx, 1, true);
        }
      }
      // Stem
      r.fillRect(cx - 1, cy + sz / 2, 3, sz, true);
      break;
    }
    case 1: {  // Hearts: two bumps on top + downward point
      // Two top bumps
      int bumpR = sz * 2 / 3;
      if (bumpR < 2) bumpR = 2;
      for (int dy = -bumpR; dy <= 0; dy++) {
        int dx = bumpR * bumpR - dy * dy;
        // approximate sqrt with iterative narrowing
        int w = 0;
        while ((w + 1) * (w + 1) <= dx) w++;
        if (w > 0) {
          r.fillRect(cx - sz / 2 - w, cy - sz / 3 + dy, w * 2 + 1, 1, true);
          r.fillRect(cx + sz / 2 - w, cy - sz / 3 + dy, w * 2 + 1, 1, true);
        }
      }
      // Downward triangle body
      for (int dy = 0; dy <= sz; dy++) {
        int halfW = sz * (sz - dy) / sz;
        if (halfW > 0) r.fillRect(cx - halfW, cy - sz / 3 + dy, halfW * 2 + 1, 1, true);
      }
      break;
    }
    case 2: {  // Diamond: expanding then contracting scanlines
      for (int dy = -sz; dy <= sz; dy++) {
        int absY = dy < 0 ? -dy : dy;
        int halfW = sz - absY;
        if (halfW > 0) r.fillRect(cx - halfW, cy + dy, halfW * 2 + 1, 1, true);
      }
      break;
    }
    case 3: {  // Clubs: three circles in clover + stem
      int cr = sz * 2 / 5;
      if (cr < 2) cr = 2;
      // Top circle
      for (int dy = -cr; dy <= cr; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= cr * cr) dx++;
        if (dx > 0) r.fillRect(cx - dx, cy - sz / 2 - cr / 3 + dy, dx * 2 + 1, 1, true);
        else r.drawPixel(cx, cy - sz / 2 - cr / 3 + dy, true);
      }
      // Bottom-left circle
      for (int dy = -cr; dy <= cr; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= cr * cr) dx++;
        if (dx > 0) r.fillRect(cx - sz / 2 - dx, cy + cr / 3 + dy, dx * 2 + 1, 1, true);
        else r.drawPixel(cx - sz / 2, cy + cr / 3 + dy, true);
      }
      // Bottom-right circle
      for (int dy = -cr; dy <= cr; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= cr * cr) dx++;
        if (dx > 0) r.fillRect(cx + sz / 2 - dx, cy + cr / 3 + dy, dx * 2 + 1, 1, true);
        else r.drawPixel(cx + sz / 2, cy + cr / 3 + dy, true);
      }
      // Stem
      r.fillRect(cx - 1, cy + cr / 2, 3, sz, true);
      break;
    }
  }
}

void CasinoActivity::drawCard(int x, int y, const Card& c, bool faceDown) {
  int cw = 70, ch = 100;
  renderer.fillRect(x + 2, y + 2, cw, ch, true);   // shadow
  renderer.fillRect(x, y, cw, ch, false);            // white face
  renderer.drawRect(x, y, cw, ch);
  renderer.drawRect(x + 1, y + 1, cw - 2, ch - 2);

  if (faceDown) {
    // Crosshatch pattern
    for (int i = 6; i < cw - 6; i += 4) {
      renderer.drawLine(x + i, y + 6, x + i, y + ch - 6);
    }
    for (int j = 6; j < ch - 6; j += 4) {
      renderer.drawLine(x + 6, y + j, x + cw - 6, y + j);
    }
    return;
  }

  // Rank top-left
  renderer.drawText(UI_10_FONT_ID, x + 4, y + 3, RANK_CHARS[c.rank]);
  // Small suit shape under rank text
  drawSuitShape(renderer, x + 12, y + 28, 5, c.suit);
  // Large suit shape centered in card
  drawSuitShape(renderer, x + cw / 2, y + ch / 2 + 4, 13, c.suit);
  // Rank bottom-right
  int rw = renderer.getTextWidth(UI_10_FONT_ID, RANK_CHARS[c.rank]);
  renderer.drawText(UI_10_FONT_ID, x + cw - rw - 4, y + ch - 22, RANK_CHARS[c.rank]);
}

// ================================================================
// SLOT MACHINE ICON DRAWING
// ================================================================

static void drawIcon7(GfxRenderer& r, int cx, int cy) {
  // Thick "7" shape
  r.fillRect(cx - 18, cy - 25, 36, 7, true);
  r.fillRect(cx - 18, cy - 18, 7, 5, true);
  for (int i = 0; i < 40; i++) {
    int px = cx + 14 - i * 28 / 40;
    int py = cy - 18 + i;
    r.fillRect(px - 3, py, 7, 1, true);
  }
}

static void drawIconBar(GfxRenderer& r, int cx, int cy) {
  r.fillRect(cx - 30, cy - 14, 60, 28, true);
  r.fillRect(cx - 27, cy - 11, 54, 22, false);
  r.drawRect(cx - 27, cy - 11, 54, 22);
  int tw = r.getTextWidth(UI_10_FONT_ID, "BAR", EpdFontFamily::BOLD);
  r.drawText(UI_10_FONT_ID, cx - tw / 2, cy - 8, "BAR", true, EpdFontFamily::BOLD);
}

static void drawIconCherry(GfxRenderer& r, int cx, int cy) {
  int cr = 10;
  int offX = 10;
  int offY = 6;
  // Left cherry
  for (int dy = -cr; dy <= cr; dy++) {
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= cr * cr) dx++;
    r.fillRect(cx - offX - dx, cy + offY + dy, dx * 2 + 1, 1, true);
  }
  // Right cherry
  for (int dy = -cr; dy <= cr; dy++) {
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= cr * cr) dx++;
    r.fillRect(cx + offX - dx, cy + offY + dy, dx * 2 + 1, 1, true);
  }
  // Stems
  for (int i = 0; i < 20; i++) {
    int sx = cx - offX + i * offX / 20;
    int sy = cy + offY - cr - i;
    r.fillRect(sx, sy, 3, 1, true);
  }
  for (int i = 0; i < 20; i++) {
    int sx = cx + offX - i * offX / 20;
    int sy = cy + offY - cr - i;
    r.fillRect(sx, sy, 3, 1, true);
  }
  // Small leaf at top
  for (int dy = -3; dy <= 3; dy++) {
    int w = 3 - (dy < 0 ? -dy : dy);
    if (w > 0) r.fillRect(cx + 2, cy - 16 + dy, w * 2, 1, true);
  }
}

static void drawIconBell(GfxRenderer& r, int cx, int cy) {
  r.fillRect(cx - 2, cy - 26, 5, 5, true);
  for (int dy = -20; dy <= 8; dy++) {
    int halfW = 6 + (dy + 20) * 16 / 28;
    r.fillRect(cx - halfW, cy + dy, halfW * 2 + 1, 1, true);
  }
  r.fillRect(cx - 25, cy + 9, 51, 5, true);
  r.fillRect(cx - 3, cy + 14, 7, 7, true);
}

static void drawIconStar(GfxRenderer& r, int cx, int cy) {
  static constexpr int N = 10;
  static constexpr int vx[N] = {0, 5, 21, 8, 14, 0, -14, -8, -21, -5};
  static constexpr int vy[N] = {-22, -7, -7, 3, 18, 9, 18, 3, -7, -7};

  int minY = -24, maxY = 20;
  for (int sy = minY; sy <= maxY; sy++) {
    int xmin = 999, xmax = -999;
    for (int i = 0; i < N; i++) {
      int j = (i + 1) % N;
      int y0 = vy[i], y1 = vy[j];
      int x0 = vx[i], x1 = vx[j];
      if ((y0 <= sy && y1 > sy) || (y1 <= sy && y0 > sy)) {
        int ix = x0 + (sy - y0) * (x1 - x0) / (y1 - y0);
        if (ix < xmin) xmin = ix;
        if (ix > xmax) xmax = ix;
      }
    }
    if (xmin <= xmax) {
      r.fillRect(cx + xmin, cy + sy, xmax - xmin + 1, 1, true);
    }
  }
}

static void drawIconDiamond(GfxRenderer& r, int cx, int cy) {
  int crownTop = cy - 22;
  int girdle = cy - 4;
  int pavilionBottom = cy + 22;
  int topHalfW = 12;
  int girdleHalfW = 28;

  for (int y = crownTop; y <= girdle; y++) {
    int t = y - crownTop;
    int h = girdle - crownTop;
    int halfW = topHalfW + (girdleHalfW - topHalfW) * t / h;
    r.fillRect(cx - halfW, y, halfW * 2 + 1, 1, true);
  }
  for (int y = girdle + 1; y <= pavilionBottom; y++) {
    int t = y - girdle;
    int h = pavilionBottom - girdle;
    int halfW = girdleHalfW * (h - t) / h;
    if (halfW > 0) r.fillRect(cx - halfW, y, halfW * 2 + 1, 1, true);
  }
  r.drawLine(cx, crownTop, cx - girdleHalfW, girdle);
  r.drawLine(cx, crownTop, cx + girdleHalfW, girdle);
  r.drawLine(cx - girdleHalfW, girdle, cx + girdleHalfW, girdle);
  r.drawLine(cx - girdleHalfW, girdle, cx, pavilionBottom);
  r.drawLine(cx + girdleHalfW, girdle, cx, pavilionBottom);
  r.drawLine(cx - topHalfW, crownTop, cx - girdleHalfW / 2, girdle);
  r.drawLine(cx + topHalfW, crownTop, cx + girdleHalfW / 2, girdle);
}

// New icons for Fruit Frenzy and wild

static void drawIconLemon(GfxRenderer& r, int cx, int cy) {
  // Oval lemon shape: wide ellipse with small nub at right end
  int hw = 26;  // half-width
  int hh = 18;  // half-height
  for (int dy = -hh; dy <= hh; dy++) {
    // Ellipse: x^2/hw^2 + y^2/hh^2 <= 1  =>  x <= hw * sqrt(1 - (dy/hh)^2)
    int dx = 0;
    while (dx < hw) {
      int lhs = dx * dx * hh * hh + dy * dy * hw * hw;
      if (lhs > hw * hw * hh * hh) break;
      dx++;
    }
    if (dx > 0) r.fillRect(cx - dx, cy + dy, dx * 2, 1, true);
  }
  // Small nub at right end (top)
  r.fillRect(cx + hw - 4, cy - hh - 4, 6, 5, true);
  r.fillRect(cx + hw - 2, cy - hh - 7, 3, 4, true);
}

static void drawIconOrange(GfxRenderer& r, int cx, int cy) {
  // Large filled circle
  int cr = 22;
  int drawCy = cy + 2;
  for (int dy = -cr; dy <= cr; dy++) {
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= cr * cr) dx++;
    if (dx > 0) r.fillRect(cx - dx, drawCy + dy, dx * 2 + 1, 1, true);
  }
  // Small stem on top
  r.fillRect(cx - 1, drawCy - cr - 5, 3, 6, true);
  // Leaf to the right of stem
  for (int dy = -3; dy <= 3; dy++) {
    int w = 4 - (dy < 0 ? -dy : dy);
    if (w > 0) r.fillRect(cx + 2, drawCy - cr - 3 + dy, w, 1, true);
  }
  // White highlight dot (inset)
  r.fillRect(cx - cr / 2 - 2, drawCy - cr / 2, 6, 4, false);
}

static void drawIconGrape(GfxRenderer& r, int cx, int cy) {
  // Cluster of 6 small circles in 1-2-3 pyramid arrangement
  int gr = 8;  // grape circle radius
  int sp = gr * 2 + 2;  // spacing

  // Row 1 (top): 1 grape
  {
    int gcy = cy - sp;
    for (int dy = -gr; dy <= gr; dy++) {
      int dx = 0;
      while ((dx + 1) * (dx + 1) + dy * dy <= gr * gr) dx++;
      if (dx > 0) r.fillRect(cx - dx, gcy + dy, dx * 2 + 1, 1, true);
    }
  }
  // Row 2 (middle): 2 grapes
  {
    int gcy = cy;
    int offX = sp / 2;
    for (int gi = 0; gi < 2; gi++) {
      int gcx = cx + (gi == 0 ? -offX : offX);
      for (int dy = -gr; dy <= gr; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= gr * gr) dx++;
        if (dx > 0) r.fillRect(gcx - dx, gcy + dy, dx * 2 + 1, 1, true);
      }
    }
  }
  // Row 3 (bottom): 3 grapes
  {
    int gcy = cy + sp;
    for (int gi = 0; gi < 3; gi++) {
      int gcx = cx + (gi - 1) * sp;
      for (int dy = -gr; dy <= gr; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= gr * gr) dx++;
        if (dx > 0) r.fillRect(gcx - dx, gcy + dy, dx * 2 + 1, 1, true);
      }
    }
  }
  // Small stem at top
  r.fillRect(cx - 1, cy - sp - gr - 5, 3, 6, true);
}

static void drawIconMelon(GfxRenderer& r, int cx, int cy) {
  // Half circle (flat top, dome at bottom) with stripe lines
  int cr = 24;
  int topY = cy - 6;
  // Flat top bar
  r.fillRect(cx - cr, topY, cr * 2 + 1, 3, true);
  // Dome: lower half circle
  for (int dy = 0; dy <= cr; dy++) {
    // For a circle centered at topY with radius cr, x = sqrt(cr^2 - dy^2)
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= cr * cr) dx++;
    if (dx > 0) r.fillRect(cx - dx, topY + dy, dx * 2 + 1, 1, true);
  }
  // Three vertical stripes (white lines on black dome)
  for (int si = 0; si < 3; si++) {
    int sx = cx - cr / 2 + si * (cr / 2);
    // Find vertical span of dome at this x
    int startY = topY;
    int endY = topY;
    for (int dy = 0; dy <= cr; dy++) {
      int dx = 0;
      while ((dx + 1) * (dx + 1) + dy * dy <= cr * cr) dx++;
      if (sx >= cx - dx && sx <= cx + dx) endY = topY + dy;
    }
    if (endY > startY + 4) {
      r.drawLine(sx, startY + 2, sx, endY - 2);
      // Override to white
      r.fillRect(sx, startY + 2, 1, endY - startY - 4, false);
    }
  }
}

static void drawIconPlum(GfxRenderer& r, int cx, int cy) {
  // Slightly tall oval with a small stem
  int hw = 20;
  int hh = 24;
  int drawCy = cy + 2;
  for (int dy = -hh; dy <= hh; dy++) {
    int dx = 0;
    while (dx < hw) {
      int lhs = dx * dx * hh * hh + dy * dy * hw * hw;
      if (lhs > hw * hw * hh * hh) break;
      dx++;
    }
    if (dx > 0) r.fillRect(cx - dx, drawCy + dy, dx * 2, 1, true);
  }
  // Stem
  r.fillRect(cx - 1, drawCy - hh - 5, 3, 6, true);
  // Small highlight
  r.fillRect(cx - hw / 2, drawCy - hh / 2, 5, 4, false);
}

static void drawIcon7Outline(GfxRenderer& r, int cx, int cy) {
  // Outlined "7" — same geometry as drawIcon7 but using draw instead of fill
  // Top horizontal bar (outlined as rect)
  r.drawRect(cx - 18, cy - 25, 36, 7);
  // Left serif (outlined)
  r.drawRect(cx - 18, cy - 18, 7, 5);
  // Diagonal stroke outline (draw only edge pixels of the thick diagonal)
  for (int i = 0; i < 40; i++) {
    int px = cx + 14 - i * 28 / 40;
    int py = cy - 18 + i;
    // Draw just the outline of the 7-pixel-wide stroke
    r.drawPixel(px - 3, py, true);
    r.drawPixel(px + 3, py, true);
  }
  // Top and bottom edge of diagonal
  r.drawPixel(cx + 14 - 3, cy - 18, true);
  r.drawPixel(cx + 14 + 3, cy - 18, true);
  r.drawPixel(cx - 14 - 3, cy + 22, true);
  r.drawPixel(cx - 14 + 3, cy + 22, true);
}

// Draw ellipse outline (3px thick ring) for coin animation
static void drawCoinEllipse(GfxRenderer& r, int cx, int cy, int radiusX, int radiusY) {
  if (radiusX < 2) {
    // Edge-on: just draw a vertical line
    r.fillRect(cx - 1, cy - radiusY, 3, radiusY * 2, true);
    return;
  }
  for (int dy = -radiusY; dy <= radiusY; dy++) {
    float t = 1.0f - (float)(dy * dy) / (float)(radiusY * radiusY);
    if (t < 0.0f) t = 0.0f;
    int outerW = (int)(radiusX * sqrtf(t));
    int innerW = (int)((radiusX - 3) * sqrtf(t));
    if (innerW < 0) innerW = 0;
    if (outerW > innerW) {
      // Left arc
      r.fillRect(cx - outerW, cy + dy, outerW - innerW, 1, true);
      // Right arc
      r.fillRect(cx + innerW + 1, cy + dy, outerW - innerW, 1, true);
    }
  }
}

void CasinoActivity::drawSlotIcon(int cx, int cy, uint8_t symbolId) {
  switch (symbolId) {
    case 0:  drawIcon7(renderer, cx, cy);        break;
    case 1:  drawIconBar(renderer, cx, cy);      break;
    case 2:  drawIconCherry(renderer, cx, cy);   break;
    case 3:  drawIconBell(renderer, cx, cy);     break;
    case 4:  drawIconStar(renderer, cx, cy);     break;
    case 5:  drawIconDiamond(renderer, cx, cy);  break;
    case 6:  drawIconLemon(renderer, cx, cy);    break;
    case 7:  drawIconOrange(renderer, cx, cy);   break;
    case 8:  drawIconGrape(renderer, cx, cy);    break;
    case 9:  drawIconMelon(renderer, cx, cy);    break;
    case 10: drawIconPlum(renderer, cx, cy);     break;
    case 11: drawIcon7Outline(renderer, cx, cy); break;
    default: break;
  }
}

void CasinoActivity::drawReelBlur(int x, int y, int w, int h, int frameOffset) {
  int bandH = 14;
  int shift = (frameOffset * 8) % (bandH * 2);
  for (int by = 0; by < h; by += bandH) {
    int drawY = y + ((by + shift) % h);
    int drawH = std::min(bandH, y + h - drawY);
    if (drawH > 0) {
      Color c = ((by / bandH) % 2 == 0) ? DarkGray : LightGray;
      renderer.fillRectDither(x + 4, drawY, w - 8, drawH, c);
    }
  }
}

void CasinoActivity::drawSlotReel(int x, int y, int symbolId, bool held, bool blur, int blurFrame) {
  int rw = 130, rh = 130;
  renderer.fillRect(x + 3, y + 3, rw, rh, true);   // shadow
  renderer.fillRect(x, y, rw, rh, false);            // white
  renderer.drawRect(x, y, rw, rh);
  renderer.drawRect(x + 2, y + 2, rw - 4, rh - 4);

  int cx = x + rw / 2;
  int cy = y + rh / 2;

  if (blur) {
    drawReelBlur(x + 3, y + 3, rw - 6, rh - 6, blurFrame);
  } else {
    drawSlotIcon(cx, cy, (uint8_t)symbolId);
  }

  // "HOLD" indicator at bottom of reel box
  if (held) {
    renderer.fillRect(x + 2, y + rh - 20, rw - 4, 18, true);
    int tw = renderer.getTextWidth(SMALL_FONT_ID, "HOLD");
    renderer.drawText(SMALL_FONT_ID, cx - tw / 2, y + rh - 17, "HOLD", false);
  }
}

// ================================================================
// SLOT MACHINE
// ================================================================

void CasinoActivity::slotsLoop() {
  switch (slotsScreen) {
    case SLOTS_MENU:    slotsMenuLoop();    break;
    case SLOTS_PLAY:    slotsPlayLoop();    break;
    case SLOTS_PAYOUTS: slotsPayoutsLoop(); break;
    case SLOTS_POWERUPS: slotsPowerupsLoop(); break;
  }
}

void CasinoActivity::slotsMenuLoop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    screen = LOBBY;
    requestUpdate();
    return;
  }
  buttonNavigator.onNext([this] {
    slotsMachineMenuIndex = ButtonNavigator::nextIndex(slotsMachineMenuIndex, NUM_MACHINES);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    slotsMachineMenuIndex = ButtonNavigator::previousIndex(slotsMachineMenuIndex, NUM_MACHINES);
    requestUpdate();
  });
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    slotsMachineIndex = slotsMachineMenuIndex;
    slotsScreen = SLOTS_PLAY;
    slotsPlayState = SP_BET;
    // Set bet to at least the machine's minimum
    const auto& machine = MACHINES[slotsMachineIndex];
    while (betIndex < NUM_BETS - 1 && BET_OPTIONS[betIndex] < machine.minBet) betIndex++;
    // Reset powerups and holds
    slotsPowerups = SlotsPowerups{};
    for (int i = 0; i < 3; i++) reelHold[i] = false;
    holdCursor = 0;
    requestUpdate();
  }
}

void CasinoActivity::slotsPlayLoop() {
  const auto& machine = MACHINES[slotsMachineIndex];

  if (slotsPlayState == SP_BET) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      slotsScreen = SLOTS_MENU;
      slotsShowLastResult = false;
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::PageForward)) {
      slotsScreen = SLOTS_PAYOUTS;
      slotsShowLastResult = false;
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      slotsScreen = SLOTS_POWERUPS;
      slotsShowLastResult = false;
      powerupMenuIndex = 0;
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (betIndex > 0 && BET_OPTIONS[betIndex - 1] >= machine.minBet) betIndex--;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (betIndex < NUM_BETS - 1) betIndex++;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      slotsShowLastResult = false;
      bool isFreeSpinActive = slotsPowerups.freeSpins > 0;
      if (!isFreeSpinActive && credits < currentBet()) {
        if (credits <= 0) { credits = 1000; saveCredits(); }
        requestUpdate();
        return;
      }
      if (!isFreeSpinActive) {
        credits -= currentBet();
      } else {
        slotsPowerups.freeSpins--;  // consume the free spin
      }
      if (machine.hasHoldReel) {
        slotsPlayState = SP_HOLD_SELECT;
        holdCursor = 0;
      } else {
        slotsSpin();
      }
      requestUpdate();
    }
    return;
  }

  if (slotsPlayState == SP_HOLD_SELECT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (holdCursor > 0) holdCursor--;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (holdCursor < 2) holdCursor++;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      reelHold[holdCursor] = !reelHold[holdCursor];
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::PageForward) ||
        mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      slotsSpin();
    }
    return;
  }

  if (slotsPlayState == SP_SPIN) {
    unsigned long elapsed = millis() - animStartMs;
    int frame = (int)(elapsed / SPIN_FRAME_MS);
    if (frame >= SPIN_FRAMES) {
      // All reels stopped — use pre-computed final values
      for (int i = 0; i < 3; i++) reels[i] = finalReels[i];
      slotsEvaluate();
    } else if (frame != animFrame) {
      animFrame = frame;
      const auto& mach = MACHINES[slotsMachineIndex];
      for (int i = 0; i < 3; i++) {
        if (reelHold[i] || frame >= reelStopFrame[i]) {
          reels[i] = finalReels[i];  // locked to final value
        } else {
          reels[i] = esp_random() % mach.numSymbols;  // still spinning
        }
      }
      requestUpdate();
    }
    return;
  }
}

void CasinoActivity::slotsPayoutsLoop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::PageForward)) {
    slotsScreen = SLOTS_PLAY;
    requestUpdate();
  }
}

void CasinoActivity::slotsPowerupsLoop() {
  static constexpr int NUM_POWERUP_ITEMS = 3;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    slotsScreen = SLOTS_PLAY;
    requestUpdate();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    if (powerupMenuIndex > 0) powerupMenuIndex--;
    requestUpdate();
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    if (powerupMenuIndex < NUM_POWERUP_ITEMS - 1) powerupMenuIndex++;
    requestUpdate();
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    const auto& machine = MACHINES[slotsMachineIndex];
    switch (powerupMenuIndex) {
      case 0:  // 2x Multiplier — 50 credits
        if (credits >= 50) {
          credits -= 50;
          slotsPowerups.multiplier = 2;
          saveCredits();
          slotsScreen = SLOTS_PLAY;
          requestUpdate();
        }
        break;
      case 1:  // Free Spin — 30 credits
        if (credits >= 30) {
          credits -= 30;
          slotsPowerups.freeSpins++;
          saveCredits();
          slotsScreen = SLOTS_PLAY;
          requestUpdate();
        }
        break;
      case 2:  // Wild Card — 40 credits (only for machines without natural wild)
        if (machine.wildSymbolIdx == 0xFF && credits >= 40) {
          credits -= 40;
          slotsPowerups.wildActive = true;
          saveCredits();
          slotsScreen = SLOTS_PLAY;
          requestUpdate();
        }
        break;
    }
  }
}

void CasinoActivity::slotsSpin() {
  // Pre-compute final reel values
  const auto& machine = MACHINES[slotsMachineIndex];
  for (int i = 0; i < 3; i++) {
    if (!reelHold[i]) finalReels[i] = esp_random() % machine.numSymbols;
    else finalReels[i] = reels[i];
  }
  slotsPlayState = SP_SPIN;
  animFrame = 0;
  animStartMs = millis();
  requestUpdate();
}

void CasinoActivity::slotsEvaluate() {
  const auto& machine = MACHINES[slotsMachineIndex];
  // Use pre-computed final reel values
  for (int i = 0; i < 3; i++) {
    reels[i] = finalReels[i];
  }
  // Reset holds after use
  for (int i = 0; i < 3; i++) reelHold[i] = false;

  int bet = currentBet();
  int win = 0;

  // Check if a reel is wild
  auto isWild = [&](int reelIdx) -> bool {
    if (machine.wildSymbolIdx != 0xFF && reels[reelIdx] == machine.wildSymbolIdx) return true;
    return false;
  };

  // Find best 3-of-a-kind match including wilds
  int bestMatchCount = 0;
  int bestSymbolIdx = -1;
  for (int s = 0; s < machine.numSymbols; s++) {
    uint8_t sid = machine.symbols.ids[s];
    if (machine.wildSymbolIdx != 0xFF && sid == machine.wildSymbolIdx) continue;
    int count = 0;
    for (int r = 0; r < 3; r++) {
      if (machine.symbols.ids[reels[r]] == sid || isWild(r)) count++;
    }
    if (count > bestMatchCount) {
      bestMatchCount = count;
      bestSymbolIdx = s;
    }
  }

  // If wildActive powerup, treat one non-matching reel as bonus count
  if (slotsPowerups.wildActive && bestMatchCount == 2) {
    bestMatchCount = 3;  // force jackpot with wild card
  }

  if (bestMatchCount >= 3 && bestSymbolIdx >= 0) {
    win = bet * machine.symbols.payouts[bestSymbolIdx];
    resultMessage = "JACKPOT!";
    // Free spin award (Fruit Frenzy: 3x Cherry = symbol index 0 in that machine)
    if (machine.hasFreeSpin && bestSymbolIdx == 0) {
      slotsPowerups.freeSpins += 3;
      resultMessage = "JACKPOT + 3 FREE SPINS!";
    }
  } else if (bestMatchCount == 2 && machine.twoMatchMult > 0) {
    win = bet * machine.twoMatchMult;
    resultMessage = "Two Match!";
  } else {
    resultMessage = "No Luck";
  }

  // Apply multiplier powerup
  if (slotsPowerups.multiplier > 1 && win > 0) {
    win *= slotsPowerups.multiplier;
    slotsPowerups.multiplier = 1;
  }

  // Consume wild powerup after use
  if (slotsPowerups.wildActive) slotsPowerups.wildActive = false;

  credits += win;
  resultAmount = win > 0 ? win : -bet;
  slotsShowLastResult = true;
  slotsPlayState = SP_BET;
  saveCredits();
  requestUpdate();
}

// ================================================================
// SLOT RENDER DISPATCH
// ================================================================

void CasinoActivity::slotsRender() {
  switch (slotsScreen) {
    case SLOTS_MENU:    slotsRenderMenu();    break;
    case SLOTS_PLAY:    slotsRenderPlay();    break;
    case SLOTS_PAYOUTS: slotsRenderPayouts(); break;
    case SLOTS_POWERUPS: slotsRenderPowerups(); break;
  }
}

void CasinoActivity::slotsRenderMenu() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.drawCenteredText(UI_12_FONT_ID, 12, "Slot Machine", true, EpdFontFamily::BOLD);
  renderer.drawLine(15, 42, pageWidth - 15, 42);
  renderCreditsBar();

  int contentTop = 90;
  int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight},
      NUM_MACHINES, slotsMachineMenuIndex,
      [](int i) { return std::string(CasinoActivity::MACHINES[i].name); },
      [](int i) { return std::string(CasinoActivity::MACHINES[i].description); });

  const auto labels = mappedInput.mapLabels("Back", "Select", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void CasinoActivity::slotsRenderPlay() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto& machine = MACHINES[slotsMachineIndex];

  // Machine name header
  renderer.drawCenteredText(UI_12_FONT_ID, 12, machine.name, true, EpdFontFamily::BOLD);
  renderer.drawLine(0, 32, pageWidth, 32);

  // Credits + Bet on same line
  char credBuf[32], betBuf[32];
  snprintf(credBuf, sizeof(credBuf), "Credits: %d", credits);
  snprintf(betBuf, sizeof(betBuf), "Bet: %d", currentBet());
  renderer.drawText(UI_10_FONT_ID, 10, 38, credBuf);
  int bw = renderer.getTextWidth(UI_10_FONT_ID, betBuf);
  renderer.drawText(UI_10_FONT_ID, pageWidth - bw - 10, 38, betBuf);
  renderer.drawLine(0, 60, pageWidth, 60);

  // 3 reels centered — 130x130 each, gap=15
  int reelW = 130, reelH = 130, gap = 15;
  int totalW = reelW * 3 + gap * 2;
  int startX = (pageWidth - totalW) / 2;
  int reelY = 75;

  bool spinning = (slotsPlayState == SP_SPIN);
  bool holdSelect = (slotsPlayState == SP_HOLD_SELECT);

  for (int i = 0; i < 3; i++) {
    // Staggered stop: reel 0 stops at frame 9, reel 1 at 10, reel 2 at 11
    bool reelDone = !spinning || (animFrame >= reelStopFrame[i]);
    bool blur = spinning && !reelDone;
    bool held = reelHold[i];
    // Show hold cursor highlight
    if (holdSelect && holdCursor == i) {
      // Draw a thicker border by drawing one extra rect
      renderer.drawRect(startX + i * (reelW + gap) - 2, reelY - 2, reelW + 4, reelH + 4);
    }
    drawSlotReel(startX + i * (reelW + gap), reelY, reels[i], held, blur, animFrame);
  }

  int infoY = reelY + reelH + 18;

  // Hold select instruction
  if (holdSelect) {
    renderer.drawCenteredText(UI_10_FONT_ID, infoY, "Select reels to HOLD");
    infoY += 22;
  }

  // Active powerup status
  bool hasPowerup = slotsPowerups.multiplier > 1 || slotsPowerups.freeSpins > 0 || slotsPowerups.wildActive;
  if (hasPowerup) {
    char pwBuf[48];
    if (slotsPowerups.freeSpins > 0) {
      snprintf(pwBuf, sizeof(pwBuf), "Free Spins: %d", slotsPowerups.freeSpins);
    } else if (slotsPowerups.multiplier > 1) {
      snprintf(pwBuf, sizeof(pwBuf), "%dx ACTIVE", slotsPowerups.multiplier);
    } else {
      snprintf(pwBuf, sizeof(pwBuf), "WILD ACTIVE");
    }
    renderer.drawCenteredText(SMALL_FONT_ID, infoY, pwBuf);
    infoY += 20;
  }

  // Spinning label
  if (spinning) {
    renderer.drawCenteredText(UI_12_FONT_ID, infoY, "SPINNING...", true, EpdFontFamily::BOLD);
  }

  // Inline result from last spin
  if (slotsShowLastResult && !spinning) {
    renderer.drawCenteredText(UI_12_FONT_ID, infoY, resultMessage.c_str(), true, EpdFontFamily::BOLD);
    infoY += 28;
    char amtBuf[32];
    if (resultAmount > 0) {
      snprintf(amtBuf, sizeof(amtBuf), "+%d credits", resultAmount);
    } else if (resultAmount < 0) {
      snprintf(amtBuf, sizeof(amtBuf), "%d credits", resultAmount);
    } else {
      snprintf(amtBuf, sizeof(amtBuf), "Push");
    }
    renderer.drawCenteredText(UI_10_FONT_ID, infoY, amtBuf);
  }

  // Button hints
  if (holdSelect) {
    const auto labels = mappedInput.mapLabels("Spin", "Toggle", "<", ">");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    const auto labels = mappedInput.mapLabels("Back", "Spin", "Bet-", "Bet+");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
}

void CasinoActivity::slotsRenderPayouts() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto& machine = MACHINES[slotsMachineIndex];

  renderer.drawCenteredText(UI_12_FONT_ID, 12, "Payout Table", true, EpdFontFamily::BOLD);
  renderer.drawLine(15, 42, pageWidth - 15, 42);

  char macBuf[48];
  snprintf(macBuf, sizeof(macBuf), "Machine: %s", machine.name);
  renderer.drawCenteredText(UI_10_FONT_ID, 55, macBuf);
  renderer.drawLine(15, 75, pageWidth - 15, 75);

  // Symbol names
  static constexpr const char* SYMBOL_DISPLAY_NAMES[] = {
      "Seven", "BAR", "Cherry", "Bell", "Star", "Diamond",
      "Lemon", "Orange", "Grape", "Melon", "Plum", "7-Wild"
  };

  int y = 88;
  for (int s = 0; s < machine.numSymbols; s++) {
    uint8_t sid = machine.symbols.ids[s];
    int8_t pay = machine.symbols.payouts[s];
    char lineBuf[48];
    const char* sname = (sid < 12) ? SYMBOL_DISPLAY_NAMES[sid] : "?";
    if (machine.wildSymbolIdx != 0xFF && sid == machine.wildSymbolIdx) {
      snprintf(lineBuf, sizeof(lineBuf), "%-8s  3x = %dx  (WILD)", sname, (int)pay);
    } else {
      snprintf(lineBuf, sizeof(lineBuf), "%-8s  3x = %dx", sname, (int)pay);
    }
    renderer.drawText(SMALL_FONT_ID, 20, y, lineBuf);
    y += 20;
  }

  if (machine.twoMatchMult > 0) {
    char twoBuf[32];
    snprintf(twoBuf, sizeof(twoBuf), "2-of-a-kind = %dx", (int)machine.twoMatchMult);
    renderer.drawText(SMALL_FONT_ID, 20, y + 4, twoBuf);
    y += 24;
  }

  if (machine.hasFreeSpin) {
    renderer.drawText(SMALL_FONT_ID, 20, y + 4, "3x top symbol = +3 Free Spins");
    y += 20;
  }
  if (machine.hasHoldReel) {
    renderer.drawText(SMALL_FONT_ID, 20, y + 4, "Hold reels between spins");
  }

  char minBuf[32];
  snprintf(minBuf, sizeof(minBuf), "Min bet: %d", (int)machine.minBet);
  renderer.drawCenteredText(SMALL_FONT_ID, 710, minBuf);

  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void CasinoActivity::slotsRenderPowerups() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto& machine = MACHINES[slotsMachineIndex];

  renderer.drawCenteredText(UI_12_FONT_ID, 12, "Powerups", true, EpdFontFamily::BOLD);
  renderer.drawLine(15, 42, pageWidth - 15, 42);

  char credBuf[32];
  snprintf(credBuf, sizeof(credBuf), "Credits: %d", credits);
  renderer.drawCenteredText(UI_10_FONT_ID, 55, credBuf);
  renderer.drawLine(15, 75, pageWidth - 15, 75);

  static constexpr int NUM_POWERUP_ITEMS = 3;
  static const char* pwNames[] = {
      "2x Multiplier",
      "Free Spin",
      "Wild Card"
  };
  static const char* pwDescs[] = {
      "Next win doubled  —  50 cr",
      "One free spin     —  30 cr",
      "Force 2-match win —  40 cr"
  };
  static const int pwCosts[] = {50, 30, 40};

  int y = 100;
  for (int i = 0; i < NUM_POWERUP_ITEMS; i++) {
    bool selected = (i == powerupMenuIndex);
    bool available = true;
    if (i == 2 && machine.wildSymbolIdx != 0xFF) available = false;  // machine has own wild

    if (selected) {
      renderer.fillRect(10, y - 2, pageWidth - 20, 40, true);
    }

    char lineBuf[48];
    snprintf(lineBuf, sizeof(lineBuf), "%s", pwNames[i]);
    renderer.drawText(UI_10_FONT_ID, 20, y + 2, lineBuf, selected);

    if (!available) {
      renderer.drawText(SMALL_FONT_ID, 20, y + 20, "N/A (machine has wild)", selected);
    } else if (credits < pwCosts[i]) {
      renderer.drawText(SMALL_FONT_ID, 20, y + 20, pwDescs[i], selected);
      // Mark as too expensive
      int tw = renderer.getTextWidth(SMALL_FONT_ID, "Not enough credits");
      renderer.drawText(SMALL_FONT_ID, pageWidth - tw - 20, y + 20, "Not enough credits", selected);
    } else {
      renderer.drawText(SMALL_FONT_ID, 20, y + 20, pwDescs[i], selected);
    }

    y += 50;
  }

  const auto labels = mappedInput.mapLabels("Back", "Buy", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ================================================================
// BLACKJACK
// ================================================================

void CasinoActivity::bjShuffle() {
  int idx = 0;
  for (uint8_t s = 0; s < 4; s++) {
    for (uint8_t r = 1; r <= 13; r++) {
      deck[idx++] = {r, s};
    }
  }
  // Fisher-Yates shuffle
  for (int i = 51; i > 0; i--) {
    int j = esp_random() % (i + 1);
    Card tmp = deck[i]; deck[i] = deck[j]; deck[j] = tmp;
  }
  deckPos = 0;
}

CasinoActivity::Card CasinoActivity::bjDraw() {
  if (deckPos >= 52) bjShuffle();
  return deck[deckPos++];
}

int CasinoActivity::bjHandValue(const std::vector<Card>& hand) const {
  int total = 0, aces = 0;
  for (auto& c : hand) {
    if (c.rank == 1) { total += 11; aces++; }
    else if (c.rank >= 10) total += 10;
    else total += c.rank;
  }
  while (total > 21 && aces > 0) { total -= 10; aces--; }
  return total;
}

bool CasinoActivity::bjIsBlackjack(const std::vector<Card>& hand) const {
  return hand.size() == 2 && bjHandValue(hand) == 21;
}

std::string CasinoActivity::bjCardStr(const Card& c) const {
  return std::string(RANK_CHARS[c.rank]) + SUIT_CHARS[c.suit];
}

void CasinoActivity::bjDeal() {
  playerHand.clear();
  dealerHand.clear();
  dealerRevealed = false;

  playerHand.push_back(bjDraw());
  dealerHand.push_back(bjDraw());
  playerHand.push_back(bjDraw());
  dealerHand.push_back(bjDraw());

  bjState = BJ_PLAYING;

  // Check for natural blackjack
  if (bjIsBlackjack(playerHand)) {
    dealerRevealed = true;
    bjEvaluate();
  }
}

void CasinoActivity::bjDealerPlay() {
  dealerRevealed = true;
  bjState = BJ_DEALER;

  while (bjHandValue(dealerHand) < 17) {
    dealerHand.push_back(bjDraw());
  }

  bjEvaluate();
}

void CasinoActivity::bjEvaluate() {
  int pVal = bjHandValue(playerHand);
  int dVal = bjHandValue(dealerHand);
  int bet = currentBet();

  if (pVal > 21) {
    resultMessage = "BUST!";
    resultAmount = -bet;
  } else if (bjIsBlackjack(playerHand) && !bjIsBlackjack(dealerHand)) {
    int win = bet * 5 / 2;  // 2.5x
    credits += win;
    resultMessage = "BLACKJACK!";
    resultAmount = win;
  } else if (dVal > 21) {
    credits += bet * 2;
    resultMessage = "Dealer Busts!";
    resultAmount = bet;
  } else if (pVal > dVal) {
    credits += bet * 2;
    resultMessage = "You Win!";
    resultAmount = bet;
  } else if (pVal == dVal) {
    credits += bet;  // push — return bet
    resultMessage = "Push";
    resultAmount = 0;
  } else {
    resultMessage = "Dealer Wins";
    resultAmount = -bet;
  }

  showingResult = true;
  bjState = BJ_BET;
  saveCredits();
  requestUpdate();
}

void CasinoActivity::bjLoop() {
  if (bjState == BJ_BET) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      screen = LOBBY; requestUpdate(); return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (betIndex > 0) betIndex--;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (betIndex < NUM_BETS - 1) betIndex++;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (credits < BET_OPTIONS[0]) {
        resetCredits();
        requestUpdate();
      } else if (credits >= currentBet()) {
        credits -= currentBet();
        bjDeal();
        requestUpdate();
      }
    }
    return;
  }

  if (bjState == BJ_PLAYING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      // Hit
      playerHand.push_back(bjDraw());
      if (bjHandValue(playerHand) > 21) {
        dealerRevealed = true;
        bjEvaluate();
      }
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Right) ||
        mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      // Stand
      bjDealerPlay();
      requestUpdate();
    }
    return;
  }
}

void CasinoActivity::bjRender() {
  const auto pageWidth = renderer.getScreenWidth();
  renderer.drawCenteredText(UI_12_FONT_ID, 12, "Blackjack", true, EpdFontFamily::BOLD);
  renderer.drawLine(15, 42, pageWidth - 15, 42);
  renderCreditsBar();

  if (bjState == BJ_BET) {
    renderer.drawCenteredText(UI_10_FONT_ID, 160, "Place your bet");
    renderBetSelector(220);
    if (credits < BET_OPTIONS[0]) {
      renderer.drawCenteredText(UI_10_FONT_ID, 310, "Broke! Press OK to reset");
    }
    const auto labels = mappedInput.mapLabels("Back", "Deal", "Bet-", "Bet+");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }

  // Dealer hand — cards are 70px wide, stride 78px
  renderer.drawText(UI_10_FONT_ID, 15, 85, "Dealer:", true, EpdFontFamily::BOLD);
  for (int i = 0; i < (int)dealerHand.size(); i++) {
    bool hidden = (!dealerRevealed && i == 1);
    drawCard(15 + i * 78, 108, dealerHand[i], hidden);
  }
  if (dealerRevealed || dealerHand.size() > 0) {
    int dVal = dealerRevealed ? bjHandValue(dealerHand) : bjHandValue({dealerHand[0]});
    char buf[16];
    snprintf(buf, sizeof(buf), "= %d%s", dVal, dealerRevealed ? "" : " + ?");
    renderer.drawText(UI_10_FONT_ID, 15 + (int)dealerHand.size() * 78 + 10, 148, buf);
  }

  // Player hand
  renderer.drawText(UI_10_FONT_ID, 15, 240, "You:", true, EpdFontFamily::BOLD);
  for (int i = 0; i < (int)playerHand.size(); i++) {
    drawCard(15 + i * 78, 263, playerHand[i]);
  }
  {
    int pVal = bjHandValue(playerHand);
    char buf[16];
    snprintf(buf, sizeof(buf), "= %d", pVal);
    renderer.drawText(UI_10_FONT_ID, 15 + (int)playerHand.size() * 78 + 10, 303, buf, true, EpdFontFamily::BOLD);
  }

  if (bjState == BJ_PLAYING) {
    const auto labels = mappedInput.mapLabels("Stand", "Hit", "", "Stand");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
}

// ================================================================
// COIN FLIP
// ================================================================

void CasinoActivity::coinLoop() {
  if (coinState == COIN_BET) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      screen = LOBBY; requestUpdate(); return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (betIndex > 0) betIndex--;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (betIndex < NUM_BETS - 1) betIndex++;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (credits < BET_OPTIONS[0]) {
        resetCredits();
        requestUpdate();
      } else if (credits >= currentBet()) {
        coinState = COIN_PICK;
        requestUpdate();
      }
    }
    return;
  }

  if (coinState == COIN_PICK) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      coinPick = 0; requestUpdate();  // Heads
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      coinPick = 1; requestUpdate();  // Tails
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      coinFlip();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      coinState = COIN_BET; requestUpdate();
    }
    return;
  }

  if (coinState == COIN_FLIPPING) {
    unsigned long elapsed = millis() - coinAnimStartMs;
    int frame = (int)(elapsed / COIN_FRAME_MS);
    if (frame >= COIN_ANIM_FRAMES) {
      // Animation done — resolve bet
      int bet = currentBet();
      credits -= bet;
      if (coinResult == coinPick) {
        credits += bet * 2;
        resultMessage = coinResult == 0 ? "HEADS - You Win!" : "TAILS - You Win!";
        resultAmount = bet;
      } else {
        resultMessage = coinResult == 0 ? "HEADS - You Lose" : "TAILS - You Lose";
        resultAmount = -bet;
      }
      showingResult = true;
      coinState = COIN_BET;
      saveCredits();
      requestUpdate();
    } else if (frame != coinAnimFrame) {
      coinAnimFrame = frame;
      requestUpdate();
    }
    return;
  }
}

void CasinoActivity::coinFlip() {
  coinResult = esp_random() % 2;  // pre-compute result
  coinAnimFrame = 0;
  coinAnimStartMs = millis();
  coinState = COIN_FLIPPING;
  requestUpdate();
}

void CasinoActivity::coinRender() {
  const auto pageWidth = renderer.getScreenWidth();
  renderer.drawCenteredText(UI_12_FONT_ID, 12, "Coin Flip", true, EpdFontFamily::BOLD);
  renderer.drawLine(15, 42, pageWidth - 15, 42);
  renderCreditsBar();

  if (coinState == COIN_BET) {
    renderer.drawCenteredText(UI_10_FONT_ID, 160, "Double or nothing!");
    renderBetSelector(220);
    if (credits < BET_OPTIONS[0]) {
      renderer.drawCenteredText(UI_10_FONT_ID, 310, "Broke! Press OK to reset");
    }
    const auto labels = mappedInput.mapLabels("Back", "Play", "Bet-", "Bet+");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (coinState == COIN_PICK) {
    renderer.drawCenteredText(UI_10_FONT_ID, 160, "Pick your side:");

    // Draw coin — larger square, moved up
    int cx = pageWidth / 2, cy = 260, cr = 90;
    renderer.drawRect(cx - cr, cy - cr, cr * 2, cr * 2);
    renderer.drawRect(cx - cr + 3, cy - cr + 3, cr * 2 - 6, cr * 2 - 6);
    renderer.drawCenteredText(UI_12_FONT_ID, cy - 14, coinPick == 0 ? "HEADS" : "TAILS", true, EpdFontFamily::BOLD);

    // Selection indicators
    renderer.drawCenteredText(UI_10_FONT_ID, 400, coinPick == 0 ? "> HEADS <" : "  HEADS  ");
    renderer.drawCenteredText(UI_10_FONT_ID, 440, coinPick == 1 ? "> TAILS <" : "  TAILS  ");

    char betBuf[32];
    snprintf(betBuf, sizeof(betBuf), "Bet: %d", currentBet());
    renderer.drawCenteredText(UI_10_FONT_ID, 490, betBuf);

    const auto labels = mappedInput.mapLabels("Cancel", "Flip!", "Heads", "Tails");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (coinState == COIN_FLIPPING) {
    renderer.drawCenteredText(UI_10_FONT_ID, 100, "Flipping...");

    int cx = pageWidth / 2;
    int cy = 280;
    int fullR = 80;

    // Animate coin rotation: vary horizontal radius
    int radiusX;
    switch (coinAnimFrame) {
      case 0: radiusX = fullR; break;          // full circle
      case 1: radiusX = fullR * 3 / 5; break;  // squished
      case 2: radiusX = 2; break;              // edge-on
      case 3: radiusX = fullR * 3 / 5; break;  // expanding
      default: radiusX = fullR; break;          // full
    }

    drawCoinEllipse(renderer, cx, cy, radiusX, fullR);

    // Show "?" during animation
    if (radiusX > 20) {
      renderer.drawCenteredText(UI_12_FONT_ID, cy - 14, "?", true, EpdFontFamily::BOLD);
    }

    char betBuf[32];
    snprintf(betBuf, sizeof(betBuf), "Bet: %d", currentBet());
    renderer.drawCenteredText(UI_10_FONT_ID, 420, betBuf);
  }
}

// ================================================================
// HIGHER / LOWER
// ================================================================

void CasinoActivity::hlDraw() {
  hlCurrentCard = bjDraw();  // reuse deck
}

void CasinoActivity::hlLoop() {
  if (hlState == HL_BET) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      screen = LOBBY; requestUpdate(); return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (betIndex > 0) betIndex--;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (betIndex < NUM_BETS - 1) betIndex++;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (credits < BET_OPTIONS[0]) {
        resetCredits();
        requestUpdate();
      } else if (credits >= currentBet()) {
        credits -= currentBet();
        hlPot = currentBet();
        hlStreak = 0;
        if (deckPos > 40) bjShuffle();  // reshuffle if low
        hlDraw();
        hlState = HL_PLAYING;
        requestUpdate();
      }
    }
    return;
  }

  if (hlState == HL_PLAYING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      hlGuess(true);  // Higher
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      hlGuess(false);  // Lower
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      hlCashOut();  // Take winnings
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      hlCashOut();  // Take winnings on back too
    }
    return;
  }
}

void CasinoActivity::hlGuess(bool higher) {
  hlNextCard = bjDraw();
  int cur = hlCurrentCard.rank;
  int nxt = hlNextCard.rank;

  bool correct = higher ? (nxt >= cur) : (nxt <= cur);

  if (correct) {
    hlStreak++;
    hlPot = hlPot * 3 / 2;  // 1.5x per correct guess
    hlCurrentCard = hlNextCard;
    requestUpdate();
  } else {
    // Lost everything
    resultMessage = "Wrong!";
    resultAmount = -(int)currentBet();
    showingResult = true;
    hlState = HL_BET;
    hlStreak = 0;
    saveCredits();
    requestUpdate();
  }
}

void CasinoActivity::hlCashOut() {
  credits += hlPot;
  int profit = hlPot - currentBet();

  char buf[32];
  snprintf(buf, sizeof(buf), "Cashed out! Streak: %d", hlStreak);
  resultMessage = buf;
  resultAmount = profit;
  showingResult = true;
  hlState = HL_BET;
  hlStreak = 0;
  saveCredits();
  requestUpdate();
}

void CasinoActivity::hlRender() {
  const auto pageWidth = renderer.getScreenWidth();
  renderer.drawCenteredText(UI_12_FONT_ID, 12, "Higher / Lower", true, EpdFontFamily::BOLD);
  renderer.drawLine(15, 42, pageWidth - 15, 42);
  renderCreditsBar();

  if (hlState == HL_BET) {
    renderer.drawCenteredText(UI_10_FONT_ID, 160, "Guess if the next card");
    renderer.drawCenteredText(UI_10_FONT_ID, 185, "is higher or lower!");
    renderBetSelector(260);

    if (credits < BET_OPTIONS[0]) {
      renderer.drawCenteredText(UI_10_FONT_ID, 310, "Broke! Press OK to reset");
    } else if (hlStreak > 0) {
      char buf[32];
      snprintf(buf, sizeof(buf), "Last streak: %d", hlStreak);
      renderer.drawCenteredText(SMALL_FONT_ID, 315, buf);
    }

    const auto labels = mappedInput.mapLabels("Back", "Start", "Bet-", "Bet+");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    // Show current card centered — 70px wide, so center at pageWidth/2 - 35
    drawCard(pageWidth / 2 - 35, 120, hlCurrentCard);

    // Streak and pot — below card (card bottom is at 120 + 100 = 220)
    char buf[48];
    snprintf(buf, sizeof(buf), "Streak: %d", hlStreak);
    renderer.drawCenteredText(UI_10_FONT_ID, 250, buf);

    snprintf(buf, sizeof(buf), "Pot: %d", hlPot);
    renderer.drawCenteredText(UI_12_FONT_ID, 290, buf, true, EpdFontFamily::BOLD);

    renderer.drawCenteredText(UI_10_FONT_ID, 370, "Higher or Lower?");

    const auto labels = mappedInput.mapLabels("", "Cash Out", "Higher", "Lower");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    GUI.drawSideButtonHints(renderer, "Higher", "Lower");
  }
}

// ================================================================
// ROULETTE
// ================================================================

void CasinoActivity::rlLoop() {
  if (rlState == RL_BET) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      screen = LOBBY; requestUpdate(); return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (betIndex > 0) betIndex--;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (betIndex < NUM_BETS - 1) betIndex++;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (credits < BET_OPTIONS[0]) {
        resetCredits();
        requestUpdate();
      } else if (credits >= currentBet()) {
        rlState = RL_PICK;
        requestUpdate();
      }
    }
    return;
  }

  if (rlState == RL_PICK) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      rlState = RL_BET; requestUpdate(); return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      int t = (int)rlBetType - 1;
      if (t < 0) t = RL_NUM_BET_TYPES - 1;
      rlBetType = (RLBetType)t;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      int t = (int)rlBetType + 1;
      if (t >= RL_NUM_BET_TYPES) t = 0;
      rlBetType = (RLBetType)t;
      requestUpdate();
    }
    if (rlBetType == RL_NUMBER) {
      if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
        if (rlNumber > 0) rlNumber--; else rlNumber = 36;
        requestUpdate();
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
        if (rlNumber < 36) rlNumber++; else rlNumber = 0;
        requestUpdate();
      }
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      rlSpin();
    }
    return;
  }
}

void CasinoActivity::rlSpin() {
  int bet = currentBet();
  credits -= bet;
  rlResult = (int)(esp_random() % 37);  // 0-36

  bool win = false;
  int payout = 0;

  switch (rlBetType) {
    case RL_RED:
      win = (rlResult != 0 && isRed(rlResult));
      payout = bet * 2;
      break;
    case RL_BLACK:
      win = (rlResult != 0 && !isRed(rlResult));
      payout = bet * 2;
      break;
    case RL_ODD:
      win = (rlResult != 0 && rlResult % 2 == 1);
      payout = bet * 2;
      break;
    case RL_EVEN:
      win = (rlResult != 0 && rlResult % 2 == 0);
      payout = bet * 2;
      break;
    case RL_LOW:
      win = (rlResult >= 1 && rlResult <= 18);
      payout = bet * 2;
      break;
    case RL_HIGH:
      win = (rlResult >= 19 && rlResult <= 36);
      payout = bet * 2;
      break;
    case RL_DOZ1:
      win = (rlResult >= 1 && rlResult <= 12);
      payout = bet * 3;
      break;
    case RL_DOZ2:
      win = (rlResult >= 13 && rlResult <= 24);
      payout = bet * 3;
      break;
    case RL_DOZ3:
      win = (rlResult >= 25 && rlResult <= 36);
      payout = bet * 3;
      break;
    case RL_NUMBER:
      win = (rlResult == rlNumber);
      payout = bet * 36;
      break;
  }

  char resBuf[48];
  const char* color = rlResult == 0 ? " Zero" : (isRed(rlResult) ? " Red" : " Black");
  if (win) {
    credits += payout;
    resultAmount = payout - bet;
    snprintf(resBuf, sizeof(resBuf), "WIN! %d%s", rlResult, color);
  } else {
    resultAmount = -bet;
    snprintf(resBuf, sizeof(resBuf), "Lose: %d%s", rlResult, color);
  }
  resultMessage = resBuf;

  showingResult = true;
  rlState = RL_BET;
  saveCredits();
  requestUpdate();
}

// Helper: get bet type display name
const char* CasinoActivity::rlBetName(RLBetType t, int num, char* buf, int bufsz) {
  switch (t) {
    case RL_RED:    return "Red";
    case RL_BLACK:  return "Black";
    case RL_ODD:    return "Odd";
    case RL_EVEN:   return "Even";
    case RL_LOW:    return "Low (1-18)";
    case RL_HIGH:   return "High (19-36)";
    case RL_DOZ1:   return "Dozen 1-12";
    case RL_DOZ2:   return "Dozen 13-24";
    case RL_DOZ3:   return "Dozen 25-36";
    case RL_NUMBER:
      snprintf(buf, bufsz, "Number: %d", num);
      return buf;
  }
  return "";
}

// Returns true if cell n is covered by current bet type selection
bool CasinoActivity::rlCellHighlighted(int n) const {
  if (n == 0) return false;
  switch (rlBetType) {
    case RL_RED:    return isRed(n);
    case RL_BLACK:  return !isRed(n);
    case RL_ODD:    return (n % 2 == 1);
    case RL_EVEN:   return (n % 2 == 0);
    case RL_LOW:    return (n >= 1 && n <= 18);
    case RL_HIGH:   return (n >= 19 && n <= 36);
    case RL_DOZ1:   return (n >= 1 && n <= 12);
    case RL_DOZ2:   return (n >= 13 && n <= 24);
    case RL_DOZ3:   return (n >= 25 && n <= 36);
    case RL_NUMBER: return (n == rlNumber);
  }
  return false;
}

void CasinoActivity::rlRender() {
  const auto pageWidth = renderer.getScreenWidth();

  renderer.drawCenteredText(UI_12_FONT_ID, 12, "Roulette", true, EpdFontFamily::BOLD);
  renderer.drawLine(15, 42, pageWidth - 15, 42);
  renderCreditsBar();

  if (rlState == RL_BET) {
    renderer.drawCenteredText(UI_10_FONT_ID, 160, "Place your bet");
    renderBetSelector(220);
    if (credits < BET_OPTIONS[0]) {
      renderer.drawCenteredText(UI_10_FONT_ID, 310, "Broke! Press OK to reset");
    }
    const auto labels = mappedInput.mapLabels("Back", "Play", "Bet-", "Bet+");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }

  // RL_PICK: show bet type selector + number grid

  // Bet type selector row
  char nameBuf[24];
  const char* btName = rlBetName(rlBetType, rlNumber, nameBuf, (int)sizeof(nameBuf));
  char selectorBuf[40];
  snprintf(selectorBuf, sizeof(selectorBuf), "< %s >", btName);
  renderer.drawCenteredText(UI_10_FONT_ID, 85, selectorBuf);

  // Bet amount
  char betBuf[24];
  snprintf(betBuf, sizeof(betBuf), "Bet: %d", currentBet());
  renderer.drawCenteredText(SMALL_FONT_ID, 110, betBuf);

  // Number grid: 3 columns, 13 rows (row 0 = zero spanning full width, rows 1-12 = numbers 1-36)
  // Cell dimensions: 42 wide, 30 tall
  static constexpr int CW = 42;
  static constexpr int CH = 30;
  static constexpr int COLS = 3;
  static constexpr int ROWS = 12;  // rows of numbers 1-36
  int gridX = (pageWidth - CW * COLS) / 2;
  int gridY = 130;

  // Row 0: "0" spanning full grid width
  {
    int zeroX = gridX;
    int zeroY = gridY;
    renderer.drawRect(zeroX, zeroY, CW * COLS, CH);
    renderer.drawRect(zeroX + 2, zeroY + 2, CW * COLS - 4, CH - 4);
    int tw = renderer.getTextWidth(SMALL_FONT_ID, "0");
    renderer.drawText(SMALL_FONT_ID, zeroX + (CW * COLS - tw) / 2, zeroY + 6, "0");
  }

  // Rows 1-12: numbers 1-36, 3 per row
  for (int row = 0; row < ROWS; row++) {
    for (int col = 0; col < COLS; col++) {
      int n = row * COLS + col + 1;  // 1-36
      int cx = gridX + col * CW;
      int cy = gridY + CH + row * CH;

      renderer.drawRect(cx, cy, CW, CH);

      bool red = isRed(n);
      bool highlighted = rlCellHighlighted(n);

      // Red numbers: small filled dot in top-right corner
      if (red) {
        renderer.fillRect(cx + CW - 7, cy + 2, 5, 5, true);
      }

      // Highlighted cells: filled bottom strip marker
      if (highlighted) {
        renderer.fillRect(cx + 2, cy + CH - 5, CW - 4, 3, true);
      }

      // Selected number (RL_NUMBER): double border
      if (rlBetType == RL_NUMBER && n == rlNumber) {
        renderer.drawRect(cx + 1, cy + 1, CW - 2, CH - 2);
      }

      // Number text
      char nbuf[4];
      snprintf(nbuf, sizeof(nbuf), "%d", n);
      int tw = renderer.getTextWidth(SMALL_FONT_ID, nbuf);
      renderer.drawText(SMALL_FONT_ID, cx + (CW - tw) / 2, cy + 6, nbuf);
    }
  }

  // Button hints depend on whether we're in number mode
  if (rlBetType == RL_NUMBER) {
    const auto labels = mappedInput.mapLabels("Back", "Spin!", "Type", "Type");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    GUI.drawSideButtonHints(renderer, "#-", "#+");
  } else {
    const auto labels = mappedInput.mapLabels("Back", "Spin!", "Type", "Type");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
}

// ================================================================
// LOOT BOX — ITEM DATABASE (stored in flash via static const)
// ================================================================

const CasinoActivity::LbItem CasinoActivity::LB_ITEMS[50] = {
  // COMMON (0-19)
  {"Resistor",       LB_COMMON,    0},
  {"Capacitor",      LB_COMMON,    1},
  {"LED",            LB_COMMON,    2},
  {"Battery",        LB_COMMON,    3},
  {"Antenna",        LB_COMMON,    4},
  {"USB Plug",       LB_COMMON,    5},
  {"SD Card",        LB_COMMON,    6},
  {"Floppy Disk",    LB_COMMON,    7},
  {"Mouse",          LB_COMMON,    8},
  {"Keyboard Key",   LB_COMMON,    9},
  {"Pixel Heart",    LB_COMMON,   10},
  {"Coffee Cup",     LB_COMMON,   11},
  {"Light Bulb",     LB_COMMON,   12},
  {"Wrench",         LB_COMMON,   13},
  {"Magnifier",      LB_COMMON,   14},
  {"Clock",          LB_COMMON,   15},
  {"Envelope",       LB_COMMON,   16},
  {"Star",           LB_COMMON,   17},
  {"Moon",           LB_COMMON,   18},
  {"Cloud",          LB_COMMON,   19},
  // RARE (20-34)
  {"Microchip",      LB_RARE,     20},
  {"Circuit Board",  LB_RARE,     21},
  {"Router",         LB_RARE,     22},
  {"Satellite",      LB_RARE,     23},
  {"Robot Head",     LB_RARE,     24},
  {"Game Boy",       LB_RARE,     25},
  {"Oscilloscope",   LB_RARE,     26},
  {"Soldering Iron", LB_RARE,     27},
  {"Drone",          LB_RARE,     28},
  {"VR Headset",     LB_RARE,     29},
  {"Server Rack",    LB_RARE,     30},
  {"Ethernet Jack",  LB_RARE,     31},
  {"Raspberry Pi",   LB_RARE,     32},
  {"Arduino",        LB_RARE,     33},
  {"Logic Analyzer", LB_RARE,     34},
  // EPIC (35-44)
  {"Golden Chip",    LB_EPIC,     35},
  {"Cyber Eye",      LB_EPIC,     36},
  {"Plasma Ball",    LB_EPIC,     37},
  {"Hologram",       LB_EPIC,     38},
  {"Quantum Bit",    LB_EPIC,     39},
  {"Neural Net",     LB_EPIC,     40},
  {"Infinity Loop",  LB_EPIC,     41},
  {"Crypto Key",     LB_EPIC,     42},
  {"Zero Day",       LB_EPIC,     43},
  {"Black Box",      LB_EPIC,     44},
  // LEGENDARY (45-49)
  {"The Kernel",     LB_LEGENDARY, 45},
  {"Root Shell",     LB_LEGENDARY, 46},
  {"Packet Ghost",   LB_LEGENDARY, 47},
  {"E-Ink Dragon",   LB_LEGENDARY, 48},
  {"biscuit. Logo",  LB_LEGENDARY, 49},
};

// ================================================================
// LOOT BOX — DITHERING HELPERS
// ================================================================

void CasinoActivity::lbFillDithered25(const GfxRenderer& r, int x, int y, int w, int h) {
  for (int dy = 0; dy < h; dy += 2)
    for (int dx = ((dy / 2) % 2); dx < w; dx += 2)
      r.drawPixel(x + dx, y + dy, true);
}

void CasinoActivity::lbFillDithered50(const GfxRenderer& r, int x, int y, int w, int h) {
  for (int dy = 0; dy < h; dy++)
    for (int dx = (dy % 2); dx < w; dx += 2)
      r.drawPixel(x + dx, y + dy, true);
}

void CasinoActivity::lbFillDithered75(const GfxRenderer& r, int x, int y, int w, int h) {
  for (int dy = 0; dy < h; dy++)
    for (int dx = ((dy + 1) % 2); dx < w; dx += 2)
      r.drawPixel(x + dx, y + dy, true);
  for (int dy = 1; dy < h; dy += 2)
    for (int dx = ((dy / 2) % 2); dx < w; dx += 2)
      r.drawPixel(x + dx, y + dy, true);
}

// ================================================================
// LOOT BOX — COLLECTION PERSISTENCE
// ================================================================

void CasinoActivity::lbLoadCollection() {
  auto file = Storage.open(LB_COLLECTION_SAVE_PATH);
  if (file && !file.isDirectory()) {
    file.read(lbCollected, 7);
    file.close();
  }
  lbTotalCollected = lbCountCollected();
}

void CasinoActivity::lbSaveCollection() {
  auto file = Storage.open(LB_COLLECTION_SAVE_PATH, O_WRITE | O_CREAT | O_TRUNC);
  if (file) {
    file.write(lbCollected, 7);
    file.close();
  }
}

bool CasinoActivity::lbHasItem(int id) const {
  if (id < 0 || id >= LB_ITEM_COUNT) return false;
  return (lbCollected[id / 8] >> (id % 8)) & 1;
}

void CasinoActivity::lbSetItem(int id) {
  if (id < 0 || id >= LB_ITEM_COUNT) return;
  lbCollected[id / 8] |= (1 << (id % 8));
}

int CasinoActivity::lbCountCollected() const {
  int count = 0;
  for (int i = 0; i < LB_ITEM_COUNT; i++)
    if (lbHasItem(i)) count++;
  return count;
}

// ================================================================
// LOOT BOX — GACHA LOGIC
// ================================================================

const char* CasinoActivity::lbRarityName(LbRarity r) {
  switch (r) {
    case LB_COMMON:    return "Common";
    case LB_RARE:      return "Rare";
    case LB_EPIC:      return "Epic";
    case LB_LEGENDARY: return "Legendary";
  }
  return "?";
}

CasinoActivity::LbRarity CasinoActivity::lbRollRarity(bool guaranteeRare) {
  int roll = (int)(esp_random() % 100);
  if (roll < 3) return LB_LEGENDARY;
  if (roll < 15) return LB_EPIC;
  if (roll < 40) return LB_RARE;
  if (guaranteeRare) return LB_RARE;
  return LB_COMMON;
}

int CasinoActivity::lbRollItem(bool guaranteeRare) {
  LbRarity r = lbRollRarity(guaranteeRare);
  int start, count;
  switch (r) {
    case LB_COMMON:    start = 0;  count = LB_COMMON_COUNT;    break;
    case LB_RARE:      start = 20; count = LB_RARE_COUNT;      break;
    case LB_EPIC:      start = 35; count = LB_EPIC_COUNT;      break;
    case LB_LEGENDARY: start = 45; count = LB_LEGENDARY_COUNT; break;
    default:           start = 0;  count = LB_COMMON_COUNT;    break;
  }
  return start + (int)(esp_random() % count);
}

void CasinoActivity::lbPerformSinglePull() {
  if (credits < LB_SINGLE_COST) return;
  credits -= LB_SINGLE_COST;
  lbPullCount = 1;
  lbPullResults[0] = lbRollItem(false);
  lbPullIsNew[0] = !lbHasItem(lbPullResults[0]);
  if (lbPullIsNew[0]) {
    lbSetItem(lbPullResults[0]);
    lbTotalCollected = lbCountCollected();
  } else {
    credits += 25;
  }
  saveCredits();
  lbSaveCollection();
  lbAnimState = LB_ANIM_SHAKING;
  lbAnimFrame = 0;
  lbAnimStartMs = millis();
  lbScreen = LB_PULLING;
  requestUpdate();
}

void CasinoActivity::lbPerformMultiPull() {
  if (credits < LB_MULTI_COST) return;
  credits -= LB_MULTI_COST;
  lbPullCount = LB_MULTI_COUNT;
  bool hasRareOrBetter = false;
  for (int i = 0; i < LB_MULTI_COUNT; i++) {
    bool guarantee = (i == LB_MULTI_COUNT - 1 && !hasRareOrBetter);
    lbPullResults[i] = lbRollItem(guarantee);
    lbPullIsNew[i] = !lbHasItem(lbPullResults[i]);
    if (lbPullIsNew[i]) {
      lbSetItem(lbPullResults[i]);
    } else {
      credits += 25;
    }
    if (LB_ITEMS[lbPullResults[i]].rarity >= LB_RARE) hasRareOrBetter = true;
  }
  lbTotalCollected = lbCountCollected();
  saveCredits();
  lbSaveCollection();
  lbAnimState = LB_ANIM_SHAKING;
  lbAnimFrame = 0;
  lbAnimStartMs = millis();
  lbRevealIndex = 0;
  lbScreen = LB_PULLING;
  requestUpdate();
}

// ================================================================
// LOOT BOX — MAIN LOOP
// ================================================================

void CasinoActivity::lbLoop() {
  switch (lbScreen) {
    case LB_MAIN_MENU: {
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        screen = LOBBY;
        requestUpdate();
        return;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
        lbMenuIndex = ButtonNavigator::previousIndex(lbMenuIndex, 3);
        requestUpdate();
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
        lbMenuIndex = ButtonNavigator::nextIndex(lbMenuIndex, 3);
        requestUpdate();
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        switch (lbMenuIndex) {
          case 0: lbPerformSinglePull(); break;
          case 1: lbPerformMultiPull(); break;
          case 2:
            lbScreen = LB_COLLECTION;
            lbCollectionPage = 0;
            lbCollectionCursor = 0;
            requestUpdate();
            break;
        }
      }
      break;
    }
    case LB_PULLING: {
      unsigned long elapsed = millis() - lbAnimStartMs;
      if (lbAnimState == LB_ANIM_SHAKING) {
        int frame = (int)(elapsed / LB_SHAKE_FRAME_MS);
        if (frame >= LB_SHAKE_FRAMES) {
          lbAnimState = LB_ANIM_OPENING;
          lbAnimFrame = 0;
          lbAnimStartMs = millis();
          requestUpdate();
        } else if (frame != lbAnimFrame) {
          lbAnimFrame = frame;
          requestUpdate();
        }
      } else if (lbAnimState == LB_ANIM_OPENING) {
        int frame = (int)(elapsed / LB_OPEN_FRAME_MS);
        if (frame >= LB_OPEN_FRAMES) {
          lbAnimState = LB_ANIM_REVEALED;
          if (lbPullCount == 1) {
            lbScreen = LB_REVEAL_SINGLE;
          } else {
            lbScreen = LB_REVEAL_MULTI;
            lbRevealIndex = 0;
          }
          requestUpdate();
        } else if (frame != lbAnimFrame) {
          lbAnimFrame = frame;
          requestUpdate();
        }
      }
      break;
    }
    case LB_REVEAL_SINGLE: {
      if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
          mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        lbScreen = LB_MAIN_MENU;
        requestUpdate();
      }
      break;
    }
    case LB_REVEAL_MULTI: {
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        lbScreen = LB_MAIN_MENU;
        requestUpdate();
        break;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        lbRevealIndex++;
        if (lbRevealIndex >= lbPullCount) {
          lbScreen = LB_MAIN_MENU;
        }
        requestUpdate();
      }
      break;
    }
    case LB_COLLECTION: {
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        lbScreen = LB_MAIN_MENU;
        requestUpdate();
        break;
      }
      int totalPages = (LB_ITEM_COUNT + LB_ITEMS_PER_PAGE - 1) / LB_ITEMS_PER_PAGE;
      if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
        lbCollectionCursor++;
        if (lbCollectionCursor >= LB_ITEMS_PER_PAGE) {
          lbCollectionCursor = 0;
          lbCollectionPage = (lbCollectionPage + 1) % totalPages;
        }
        int itemId = lbCollectionPage * LB_ITEMS_PER_PAGE + lbCollectionCursor;
        if (itemId >= LB_ITEM_COUNT) { lbCollectionPage = 0; lbCollectionCursor = 0; }
        requestUpdate();
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
        lbCollectionCursor--;
        if (lbCollectionCursor < 0) {
          lbCollectionCursor = LB_ITEMS_PER_PAGE - 1;
          lbCollectionPage = (lbCollectionPage - 1 + totalPages) % totalPages;
        }
        int itemId = lbCollectionPage * LB_ITEMS_PER_PAGE + lbCollectionCursor;
        if (itemId >= LB_ITEM_COUNT) {
          lbCollectionCursor = (LB_ITEM_COUNT - 1) % LB_ITEMS_PER_PAGE;
          lbCollectionPage = (LB_ITEM_COUNT - 1) / LB_ITEMS_PER_PAGE;
        }
        requestUpdate();
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
        lbCollectionCursor -= LB_GRID_COLS;
        if (lbCollectionCursor < 0) {
          lbCollectionPage = (lbCollectionPage - 1 + totalPages) % totalPages;
          lbCollectionCursor += LB_ITEMS_PER_PAGE;
          int itemId = lbCollectionPage * LB_ITEMS_PER_PAGE + lbCollectionCursor;
          if (itemId >= LB_ITEM_COUNT) {
            lbCollectionCursor = (LB_ITEM_COUNT - 1) % LB_ITEMS_PER_PAGE;
            lbCollectionPage = (LB_ITEM_COUNT - 1) / LB_ITEMS_PER_PAGE;
          }
        }
        requestUpdate();
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
        lbCollectionCursor += LB_GRID_COLS;
        if (lbCollectionCursor >= LB_ITEMS_PER_PAGE) {
          lbCollectionCursor -= LB_ITEMS_PER_PAGE;
          lbCollectionPage = (lbCollectionPage + 1) % totalPages;
        }
        int itemId = lbCollectionPage * LB_ITEMS_PER_PAGE + lbCollectionCursor;
        if (itemId >= LB_ITEM_COUNT) { lbCollectionPage = 0; lbCollectionCursor = 0; }
        requestUpdate();
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::PageForward)) {
        lbCollectionPage = (lbCollectionPage + 1) % totalPages;
        lbCollectionCursor = 0;
        requestUpdate();
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::PageBack)) {
        lbCollectionPage = (lbCollectionPage - 1 + totalPages) % totalPages;
        lbCollectionCursor = 0;
        requestUpdate();
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        int itemId = lbCollectionPage * LB_ITEMS_PER_PAGE + lbCollectionCursor;
        if (itemId < LB_ITEM_COUNT && lbHasItem(itemId)) {
          lbDetailItemId = itemId;
          lbScreen = LB_ITEM_DETAIL;
          requestUpdate();
        }
      }
      break;
    }
    case LB_ITEM_DETAIL: {
      if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
          mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        lbScreen = LB_COLLECTION;
        requestUpdate();
      }
      break;
    }
  }
}

// ================================================================
// LOOT BOX — RENDER DISPATCHER
// ================================================================

void CasinoActivity::lbRender() {
  renderer.clearScreen();

  switch (lbScreen) {
    case LB_MAIN_MENU:     lbRenderMainMenu();     break;
    case LB_PULLING:       lbRenderPulling();      break;
    case LB_REVEAL_SINGLE: lbRenderRevealSingle(); break;
    case LB_REVEAL_MULTI:  lbRenderRevealMulti();  break;
    case LB_COLLECTION:    lbRenderCollection();   break;
    case LB_ITEM_DETAIL:   lbRenderItemDetail();   break;
  }

  if (lbScreen == LB_REVEAL_SINGLE || lbScreen == LB_REVEAL_MULTI ||
      lbScreen == LB_ITEM_DETAIL   || lbScreen == LB_COLLECTION) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer();
  }
}

// ================================================================
// LOOT BOX — RENDER: MAIN MENU
// ================================================================

void CasinoActivity::lbRenderMainMenu() {
  const auto pageWidth = renderer.getScreenWidth();

  renderer.drawCenteredText(UI_12_FONT_ID, 12, "Loot Box", true, EpdFontFamily::BOLD);
  renderer.drawLine(15, 42, pageWidth - 15, 42);

  char buf[32];
  snprintf(buf, sizeof(buf), "Credits: %d", (int)credits);
  renderer.drawText(UI_10_FONT_ID, 15, 50, buf, true, EpdFontFamily::BOLD);
  renderer.drawLine(0, 72, pageWidth, 72);

  lbDrawLootBox(pageWidth / 2, 250, 140, 0);

  int menuY = 400;
  const int menuSpacing = 45;

  char singleBuf[40], multiBuf[40], collBuf[40];
  snprintf(singleBuf, sizeof(singleBuf), "Single Pull (%d)", LB_SINGLE_COST);
  snprintf(multiBuf,  sizeof(multiBuf),  "5x Pull (%d)",    LB_MULTI_COST);
  snprintf(collBuf,   sizeof(collBuf),   "Collection (%d/%d)", lbTotalCollected, LB_ITEM_COUNT);

  const char* items[] = {singleBuf, multiBuf, collBuf};
  for (int i = 0; i < 3; i++) {
    int y = menuY + i * menuSpacing;
    if (i == lbMenuIndex) {
      renderer.fillRect(30, y - 2, pageWidth - 60, 30, true);
      renderer.drawCenteredText(UI_10_FONT_ID, y, items[i], false, EpdFontFamily::BOLD);
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, y, items[i]);
    }
  }

  if (lbMenuIndex == 0 && credits < LB_SINGLE_COST) {
    renderer.drawCenteredText(SMALL_FONT_ID, menuY + 3 * menuSpacing + 10, "Not enough credits!");
  } else if (lbMenuIndex == 1 && credits < LB_MULTI_COST) {
    renderer.drawCenteredText(SMALL_FONT_ID, menuY + 3 * menuSpacing + 10, "Not enough credits!");
  }

  const auto labels = mappedInput.mapLabels("Back", "Select", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ================================================================
// LOOT BOX — RENDER: PULLING ANIMATION
// ================================================================

void CasinoActivity::lbRenderPulling() {
  const auto pageWidth  = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  int cx = pageWidth / 2;
  int cy = pageHeight / 2 - 50;

  if (lbAnimState == LB_ANIM_SHAKING) {
    static constexpr int shakeOffsets[] = {6, -6, 8, -8};
    int offset = shakeOffsets[lbAnimFrame % 4];
    lbDrawLootBox(cx, cy, 160, offset);
    renderer.drawCenteredText(UI_10_FONT_ID, cy + 120, "Opening...");
  } else if (lbAnimState == LB_ANIM_OPENING) {
    int lidOffset = (lbAnimFrame + 1) * 15;
    int x = cx - 80;
    int y = cy - 80;

    // Shadow
    lbFillDithered50(renderer, x + 4, y + 4 + 54, 160, 106);
    // Box body
    renderer.fillRect(x, y + 54, 160, 106, false);
    renderer.drawRect(x, y + 54, 160, 106);
    renderer.drawRect(x + 2, y + 56, 156, 102);
    // "?" on front
    renderer.drawCenteredText(UI_12_FONT_ID, cy + 20, "?", true, EpdFontFamily::BOLD);

    // Lid floating up
    int lidY = y - lidOffset;
    int lidX = x - 4 + (lbAnimFrame * 5);
    renderer.fillRect(lidX, lidY, 168, 54, false);
    renderer.drawRect(lidX, lidY, 168, 54);

    if (lbAnimFrame >= 2) {
      for (int i = 0; i < 8; i++) {
        float angle = i * 0.7854f;
        int ex = cx + (int)(60 * cosf(angle));
        int ey = cy + (int)(60 * sinf(angle));
        renderer.drawLine(cx, cy, ex, ey);
      }
    }
    renderer.drawCenteredText(UI_10_FONT_ID, cy + 120, "Opening...");
  }
}

// ================================================================
// LOOT BOX — RENDER: REVEAL SINGLE
// ================================================================

void CasinoActivity::lbRenderRevealSingle() {
  const auto pageWidth  = renderer.getScreenWidth();
  int itemId = lbPullResults[0];
  const LbItem& item = LB_ITEMS[itemId];
  bool isNew = lbPullIsNew[0];

  if (isNew) {
    renderer.drawCenteredText(UI_12_FONT_ID, 20, "NEW ITEM!", true, EpdFontFamily::BOLD);
  } else {
    renderer.drawCenteredText(UI_12_FONT_ID, 20, "DUPLICATE", true, EpdFontFamily::BOLD);
  }
  renderer.drawLine(15, 50, pageWidth - 15, 50);

  int frameX = pageWidth / 2 - 80;
  int frameY = 80;
  int frameW = 160;
  int frameH = 160;
  lbDrawRarityBorder(frameX, frameY, frameW, frameH, item.rarity);

  int iconCx = pageWidth / 2;
  int iconCy = frameY + frameH / 2;
  if (isNew) {
    for (int i = 0; i < 8; i++) {
      float angle = i * 0.7854f;
      int ex = iconCx + (int)(90 * cosf(angle));
      int ey = iconCy + (int)(90 * sinf(angle));
      renderer.drawLine(iconCx, iconCy, ex, ey);
    }
  }

  lbDrawItemIcon(frameX + 20, frameY + 20, frameW - 40, item.iconId, false);

  int starY = frameY + frameH + 20;
  lbDrawStars(pageWidth / 2, starY, item.rarity);

  char rarBuf[32];
  snprintf(rarBuf, sizeof(rarBuf), "~ %s ~", lbRarityName(item.rarity));
  renderer.drawCenteredText(UI_10_FONT_ID, starY + 30, rarBuf, true, EpdFontFamily::BOLD);

  renderer.drawCenteredText(UI_12_FONT_ID, starY + 65, item.name, true, EpdFontFamily::BOLD);

  if (!isNew) {
    renderer.drawCenteredText(SMALL_FONT_ID, starY + 100, "+25 credits refunded");
  }

  char progBuf[32];
  snprintf(progBuf, sizeof(progBuf), "%d/%d Collected", lbTotalCollected, LB_ITEM_COUNT);
  renderer.drawCenteredText(SMALL_FONT_ID, starY + 130, progBuf);

  const auto labels = mappedInput.mapLabels("Back", "OK", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ================================================================
// LOOT BOX — RENDER: REVEAL MULTI
// ================================================================

void CasinoActivity::lbRenderRevealMulti() {
  const auto pageWidth = renderer.getScreenWidth();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, "5x PULL RESULTS", true, EpdFontFamily::BOLD);
  renderer.drawLine(15, 45, pageWidth - 15, 45);

  int slotSize = 70;
  int spacing  = 10;
  int totalW   = LB_MULTI_COUNT * slotSize + (LB_MULTI_COUNT - 1) * spacing;
  int startX   = (pageWidth - totalW) / 2;
  int slotY    = 80;

  for (int i = 0; i < LB_MULTI_COUNT; i++) {
    int sx = startX + i * (slotSize + spacing);

    if (i <= lbRevealIndex) {
      int itemId = lbPullResults[i];
      const LbItem& item = LB_ITEMS[itemId];
      lbDrawRarityBorder(sx, slotY, slotSize, slotSize, item.rarity);
      lbDrawItemIcon(sx + 8, slotY + 8, slotSize - 16, item.iconId, false);

      // Name below slot
      const char* name = item.name;
      int tw = renderer.getTextWidth(SMALL_FONT_ID, name);
      if (tw <= slotSize + 10) {
        renderer.drawText(SMALL_FONT_ID, sx + (slotSize - tw) / 2, slotY + slotSize + 4, name);
      } else {
        char shortName[8];
        strncpy(shortName, name, 6);
        shortName[6] = '.';
        shortName[7] = '\0';
        int tw2 = renderer.getTextWidth(SMALL_FONT_ID, shortName);
        renderer.drawText(SMALL_FONT_ID, sx + (slotSize - tw2) / 2, slotY + slotSize + 4, shortName);
      }

      // NEW badge
      if (lbPullIsNew[i]) {
        renderer.fillRect(sx, slotY - 12, 28, 12, true);
        renderer.drawText(SMALL_FONT_ID, sx + 2, slotY - 12, "NEW", false);
      }

      // Star rating
      int starCount = 1;
      if (item.rarity == LB_RARE)           starCount = 2;
      else if (item.rarity == LB_EPIC)      starCount = 3;
      else if (item.rarity == LB_LEGENDARY) starCount = 5;
      char starBuf[6] = {};
      int sc = starCount < 5 ? starCount : 5;
      for (int si = 0; si < sc; si++) starBuf[si] = '*';
      starBuf[sc] = '\0';
      int stw = renderer.getTextWidth(SMALL_FONT_ID, starBuf);
      renderer.drawText(SMALL_FONT_ID, sx + (slotSize - stw) / 2, slotY + slotSize + 18, starBuf);
    } else {
      // Unrevealed: dithered box with "?"
      lbFillDithered50(renderer, sx + 2, slotY + 2, slotSize - 4, slotSize - 4);
      renderer.drawRect(sx, slotY, slotSize, slotSize);
      char qm[] = "?";
      int tw = renderer.getTextWidth(UI_12_FONT_ID, qm, EpdFontFamily::BOLD);
      renderer.fillRect(sx + 4, slotY + slotSize / 2 - 12, slotSize - 8, 24, false);
      lbFillDithered50(renderer, sx + 4, slotY + slotSize / 2 - 12, slotSize - 8, 24);
      renderer.drawText(UI_12_FONT_ID, sx + (slotSize - tw) / 2,
                        slotY + slotSize / 2 - 10, qm, true, EpdFontFamily::BOLD);
    }
  }

  char progBuf[32];
  snprintf(progBuf, sizeof(progBuf), "Revealing: %d of %d", lbRevealIndex + 1, LB_MULTI_COUNT);
  renderer.drawCenteredText(UI_10_FONT_ID, slotY + slotSize + 50, progBuf);

  if (lbRevealIndex >= LB_MULTI_COUNT - 1) {
    int newCount = 0, dupeCount = 0;
    for (int i = 0; i < LB_MULTI_COUNT; i++) {
      if (lbPullIsNew[i]) newCount++; else dupeCount++;
    }
    char sumBuf[64];
    if (dupeCount > 0) {
      snprintf(sumBuf, sizeof(sumBuf), "%d new! %d dupes (+%d credits)", newCount, dupeCount, dupeCount * 25);
    } else {
      snprintf(sumBuf, sizeof(sumBuf), "%d new items!", newCount);
    }
    renderer.drawCenteredText(UI_10_FONT_ID, slotY + slotSize + 80, sumBuf, true, EpdFontFamily::BOLD);
  }

  const char* confirmLabel = (lbRevealIndex >= LB_MULTI_COUNT - 1) ? "Done" : "Next";
  const auto labels = mappedInput.mapLabels("Back", confirmLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ================================================================
// LOOT BOX — RENDER: COLLECTION GRID
// ================================================================

void CasinoActivity::lbRenderCollection() {
  const auto pageWidth  = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  int totalPages = (LB_ITEM_COUNT + LB_ITEMS_PER_PAGE - 1) / LB_ITEMS_PER_PAGE;

  char hdrBuf[40];
  snprintf(hdrBuf, sizeof(hdrBuf), "Collection (%d/%d)", lbTotalCollected, LB_ITEM_COUNT);
  renderer.drawCenteredText(UI_12_FONT_ID, 12, hdrBuf, true, EpdFontFamily::BOLD);
  renderer.drawLine(15, 42, pageWidth - 15, 42);

  int cellSize    = 70;
  int cellSpacing = 12;
  int gridW       = LB_GRID_COLS * cellSize + (LB_GRID_COLS - 1) * cellSpacing;
  int gridStartX  = (pageWidth - gridW) / 2;
  int gridStartY  = 60;

  for (int row = 0; row < LB_GRID_ROWS; row++) {
    for (int col = 0; col < LB_GRID_COLS; col++) {
      int idx    = row * LB_GRID_COLS + col;
      int itemId = lbCollectionPage * LB_ITEMS_PER_PAGE + idx;
      if (itemId >= LB_ITEM_COUNT) continue;

      int cx = gridStartX + col * (cellSize + cellSpacing);
      int cy = gridStartY + row * (cellSize + cellSpacing + 20);

      bool owned    = lbHasItem(itemId);
      bool selected = (idx == lbCollectionCursor);

      if (selected) {
        renderer.drawRect(cx - 3, cy - 3, cellSize + 6, cellSize + 6);
        renderer.drawRect(cx - 2, cy - 2, cellSize + 4, cellSize + 4);
      }

      renderer.drawRect(cx, cy, cellSize, cellSize);
      lbDrawItemIcon(cx + 5, cy + 5, cellSize - 10, LB_ITEMS[itemId].iconId, !owned);

      if (owned) {
        const char* name = LB_ITEMS[itemId].name;
        int tw = renderer.getTextWidth(SMALL_FONT_ID, name);
        if (tw <= cellSize + 10) {
          renderer.drawText(SMALL_FONT_ID, cx + (cellSize - tw) / 2, cy + cellSize + 2, name);
        } else {
          char shortName[7];
          strncpy(shortName, name, 5);
          shortName[5] = '.';
          shortName[6] = '\0';
          int tw2 = renderer.getTextWidth(SMALL_FONT_ID, shortName);
          renderer.drawText(SMALL_FONT_ID, cx + (cellSize - tw2) / 2, cy + cellSize + 2, shortName);
        }
      } else {
        renderer.drawText(SMALL_FONT_ID, cx + cellSize / 2 - 8, cy + cellSize + 2, "???");
      }
    }
  }

  // Progress bar
  int barY  = pageHeight - 120;
  int barW  = pageWidth - 80;
  int barX  = 40;
  int barH  = 16;
  renderer.drawRect(barX, barY, barW, barH);
  int fillW = (barW - 4) * lbTotalCollected / LB_ITEM_COUNT;
  if (fillW > 0) {
    lbFillDithered50(renderer, barX + 2, barY + 2, fillW, barH - 4);
  }

  char pageBuf[16];
  snprintf(pageBuf, sizeof(pageBuf), "Page %d/%d", lbCollectionPage + 1, totalPages);
  renderer.drawCenteredText(SMALL_FONT_ID, barY + 22, pageBuf);

  const auto labels = mappedInput.mapLabels("Back", "Detail", "Left", "Right");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, "Pg-", "Pg+");
}

// ================================================================
// LOOT BOX — RENDER: ITEM DETAIL
// ================================================================

void CasinoActivity::lbRenderItemDetail() {
  if (lbDetailItemId < 0 || lbDetailItemId >= LB_ITEM_COUNT) return;
  const auto pageWidth = renderer.getScreenWidth();
  const LbItem& item = LB_ITEMS[lbDetailItemId];

  int frameX = pageWidth / 2 - 110;
  int frameY = 50;
  int frameW = 220;
  int frameH = 220;
  lbDrawRarityBorder(frameX, frameY, frameW, frameH, item.rarity);

  lbDrawItemIcon(frameX + 30, frameY + 30, frameW - 60, item.iconId, false);

  int infoY = frameY + frameH + 20;
  lbDrawStars(pageWidth / 2, infoY, item.rarity);

  char rarBuf[32];
  snprintf(rarBuf, sizeof(rarBuf), "~ %s ~", lbRarityName(item.rarity));
  renderer.drawCenteredText(UI_10_FONT_ID, infoY + 30, rarBuf, true, EpdFontFamily::BOLD);

  renderer.drawCenteredText(UI_12_FONT_ID, infoY + 65, item.name, true, EpdFontFamily::BOLD);

  char numBuf[16];
  snprintf(numBuf, sizeof(numBuf), "#%d of %d", lbDetailItemId + 1, LB_ITEM_COUNT);
  renderer.drawCenteredText(SMALL_FONT_ID, infoY + 100, numBuf);

  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ================================================================
// LOOT BOX — DRAW: LOOT BOX
// ================================================================

void CasinoActivity::lbDrawLootBox(int cx, int cy, int size, int shakeOffset) const {
  int x = cx - size / 2 + shakeOffset;
  int y = cy - size / 2;

  lbFillDithered50(renderer, x + 4, y + 4, size, size);

  // Box body
  renderer.fillRect(x, y + size / 3, size, size * 2 / 3, false);
  renderer.drawRect(x, y + size / 3, size, size * 2 / 3);
  renderer.drawRect(x + 2, y + size / 3 + 2, size - 4, size * 2 / 3 - 4);

  // Lid
  renderer.fillRect(x - 4, y, size + 8, size / 3, false);
  renderer.drawRect(x - 4, y, size + 8, size / 3);
  renderer.drawRect(x - 2, y + 2, size + 4, size / 3 - 4);

  // Ribbon cross
  renderer.fillRect(cx + shakeOffset - 2, y, 5, size, true);
  renderer.fillRect(x, cy - 2, size, 5, true);

  renderer.drawCenteredText(UI_12_FONT_ID, cy + size / 6, "?", true, EpdFontFamily::BOLD);
}

// ================================================================
// LOOT BOX — DRAW: RARITY BORDER
// ================================================================

void CasinoActivity::lbDrawRarityBorder(int x, int y, int w, int h, LbRarity rarity) const {
  switch (rarity) {
    case LB_COMMON:
      renderer.drawRect(x, y, w, h);
      break;
    case LB_RARE:
      renderer.drawRect(x, y, w, h);
      renderer.drawRect(x + 3, y + 3, w - 6, h - 6);
      break;
    case LB_EPIC:
      renderer.drawRect(x, y, w, h);
      renderer.drawRect(x + 1, y + 1, w - 2, h - 2);
      lbFillDithered25(renderer, x + 3, y + 3, w - 6, 3);
      lbFillDithered25(renderer, x + 3, y + h - 6, w - 6, 3);
      lbFillDithered25(renderer, x + 3, y + 3, 3, h - 6);
      lbFillDithered25(renderer, x + w - 6, y + 3, 3, h - 6);
      renderer.drawRect(x + 6, y + 6, w - 12, h - 12);
      break;
    case LB_LEGENDARY: {
      renderer.fillRect(x, y, w, h, true);
      renderer.fillRect(x + 4, y + 4, w - 8, h - 8, false);
      lbFillDithered50(renderer, x + 5, y + 5, w - 10, 2);
      lbFillDithered50(renderer, x + 5, y + h - 7, w - 10, 2);
      lbFillDithered50(renderer, x + 5, y + 5, 2, h - 10);
      lbFillDithered50(renderer, x + w - 7, y + 5, 2, h - 10);
      int corners[4][2] = {{x+2, y+2}, {x+w-4, y+2}, {x+2, y+h-4}, {x+w-4, y+h-4}};
      for (int ci = 0; ci < 4; ci++) {
        int cx2 = corners[ci][0];
        int cy2 = corners[ci][1];
        renderer.drawPixel(cx2 + 1, cy2,     false);
        renderer.drawPixel(cx2,     cy2 + 1, false);
        renderer.drawPixel(cx2 + 2, cy2 + 1, false);
        renderer.drawPixel(cx2 + 1, cy2 + 2, false);
      }
      break;
    }
  }
}

// ================================================================
// LOOT BOX — DRAW: STARS
// ================================================================

void CasinoActivity::lbDrawStars(int cx, int y, LbRarity rarity) const {
  int count = 1;
  if (rarity == LB_RARE)           count = 2;
  else if (rarity == LB_EPIC)      count = 3;
  else if (rarity == LB_LEGENDARY) count = 5;

  int starSpacing = 20;
  int startX = cx - (count - 1) * starSpacing / 2;

  for (int i = 0; i < count; i++) {
    int sx = startX + i * starSpacing;
    int r  = 6;
    for (int a = 0; a < 5; a++) {
      float angle = -1.5708f + a * 1.2566f;
      int tx = sx + (int)(r * cosf(angle));
      int ty = y  + (int)(r * sinf(angle));
      renderer.drawLine(sx, y, tx, ty);
    }
  }
}

// ================================================================
// LOOT BOX — DRAW: ITEM ICON
// ================================================================

void CasinoActivity::lbDrawItemIcon(int x, int y, int size, int iconId, bool locked) const {
  int cx = x + size / 2;
  int cy = y + size / 2;
  int s  = size / 3;

  if (locked) {
    lbFillDithered75(renderer, x + 2, y + 2, size - 4, size - 4);
    int lw = s, lh = s;
    renderer.fillRect(cx - lw / 2, cy, lw, lh, false);
    renderer.drawRect(cx - lw / 2, cy, lw, lh);
    renderer.drawRect(cx - lw / 4, cy - lh / 2, lw / 2, lh / 2);
    renderer.fillRect(cx - 1, cy + lh / 3, 3, 3, true);
    return;
  }

  switch (iconId) {
    case 0: { // Resistor
      renderer.drawLine(x + 4, cy, cx - s, cy);
      for (int i = 0; i < 4; i++) {
        int bx = cx - s + i * s / 2;
        renderer.drawLine(bx,          cy,          bx + s / 4, cy - s / 2);
        renderer.drawLine(bx + s / 4,  cy - s / 2,  bx + s / 2, cy + s / 2);
      }
      renderer.drawLine(cx + s, cy, x + size - 4, cy);
      break;
    }
    case 1: { // Capacitor
      renderer.fillRect(cx - s / 3 - 2, cy - s, 3, s * 2, true);
      renderer.fillRect(cx + s / 3,     cy - s, 3, s * 2, true);
      renderer.drawLine(x + 4, cy, cx - s / 3 - 2, cy);
      renderer.drawLine(cx + s / 3 + 3, cy, x + size - 4, cy);
      break;
    }
    case 2: { // LED
      renderer.drawLine(cx - s / 2, cy - s, cx - s / 2, cy + s);
      renderer.drawLine(cx - s / 2, cy - s, cx + s / 2, cy);
      renderer.drawLine(cx - s / 2, cy + s, cx + s / 2, cy);
      renderer.fillRect(cx + s / 2, cy - s, 2, s * 2, true);
      for (int i = -1; i <= 1; i++)
        renderer.drawLine(cx + s / 2 + 3, cy + i * s / 2, cx + s, cy + i * s / 2 - s / 3);
      break;
    }
    case 3: { // Battery
      renderer.drawRect(cx - s, cy - s / 2, s * 3 / 2, s);
      renderer.fillRect(cx + s / 2 + 1, cy - s / 4, s / 3, s / 2, true);
      renderer.drawLine(cx - s / 2, cy - s / 4, cx - s / 2, cy + s / 4);
      renderer.drawLine(cx - s + 3,          cy - s / 6, cx - s + 3 + s / 4, cy - s / 6);
      renderer.drawLine(cx - s + 3 + s / 8,  cy - s / 3, cx - s + 3 + s / 8, cy);
      break;
    }
    case 4: { // Antenna
      renderer.drawLine(cx, cy + s, cx, cy - s / 2);
      renderer.drawLine(cx, cy - s / 2, cx - s / 2, cy - s);
      renderer.drawLine(cx, cy - s / 2, cx + s / 2, cy - s);
      renderer.drawLine(cx + s / 2 + 2, cy - s / 3, cx + s / 2 + 4, cy - s / 2);
      renderer.drawLine(cx + s / 2 + 5, cy - s / 4, cx + s / 2 + 7, cy - s / 2);
      break;
    }
    case 5: { // USB Plug
      renderer.drawRect(cx - s / 2, cy - s, s, s * 2);
      renderer.fillRect(cx - s / 4, cy - s - s / 3, s / 2, s / 3, true);
      renderer.drawLine(cx - s / 6, cy - s / 2, cx - s / 6, cy + s / 2);
      renderer.drawLine(cx + s / 6, cy - s / 2, cx + s / 6, cy + s / 2);
      break;
    }
    case 6: { // SD Card
      renderer.drawRect(cx - s / 2, cy - s, s, s * 2);
      renderer.drawLine(cx - s / 2, cy - s, cx - s / 4, cy - s - s / 3);
      renderer.drawLine(cx - s / 4, cy - s - s / 3, cx + s / 2, cy - s - s / 3);
      renderer.fillRect(cx - s / 4, cy - s / 2, s / 2, 2, true);
      renderer.fillRect(cx - s / 4, cy,          s / 2, 2, true);
      break;
    }
    case 7: { // Floppy Disk
      renderer.drawRect(cx - s, cy - s, s * 2, s * 2);
      renderer.fillRect(cx - s / 2, cy - s, s, s / 2, true);
      renderer.fillRect(cx - s / 3, cy + s / 4, s * 2 / 3, s * 3 / 4, false);
      renderer.drawRect(cx - s / 3, cy + s / 4, s * 2 / 3, s * 3 / 4);
      break;
    }
    case 8: { // Mouse
      renderer.drawRect(cx - s / 2, cy - s, s, s * 2);
      renderer.drawLine(cx, cy - s, cx, cy - s / 3);
      renderer.drawLine(cx - s / 2, cy - s / 3, cx + s / 2, cy - s / 3);
      renderer.drawLine(cx - s / 2, cy + s, cx + s / 2, cy + s);
      break;
    }
    case 9: { // Keyboard Key
      renderer.drawRect(cx - s / 2, cy - s / 2, s, s);
      renderer.drawRect(cx - s / 2 + 2, cy - s / 2 + 2, s - 4, s - 4);
      char key[] = "A";
      int tw = renderer.getTextWidth(SMALL_FONT_ID, key);
      renderer.drawText(SMALL_FONT_ID, cx - tw / 2, cy - 5, key, true, EpdFontFamily::BOLD);
      break;
    }
    case 10: { // Pixel Heart
      for (int dy = -s; dy <= s; dy++) {
        for (int dx = -s; dx <= s; dx++) {
          int lx = dx + s / 3, ly = dy + s / 3;
          int rx = dx - s / 3, ry = dy + s / 3;
          bool lb2 = (lx * lx + ly * ly * 2) <= s * s / 2;
          bool rb2 = (rx * rx + ry * ry * 2) <= s * s / 2;
          bool tri = (dy >= 0) && (abs(dx) <= s - dy);
          if (lb2 || rb2 || tri) renderer.drawPixel(cx + dx, cy + dy, true);
        }
      }
      break;
    }
    case 11: { // Coffee Cup
      renderer.drawRect(cx - s / 2, cy - s / 2, s, s + s / 3);
      renderer.drawRect(cx + s / 2, cy - s / 4, s / 3, s / 2);
      for (int i = 0; i < 3; i++) {
        int sx2 = cx - s / 4 + i * s / 4;
        renderer.drawPixel(sx2,     cy - s / 2 - 2, true);
        renderer.drawPixel(sx2 + 1, cy - s / 2 - 4, true);
        renderer.drawPixel(sx2,     cy - s / 2 - 6, true);
      }
      break;
    }
    case 12: { // Light Bulb
      for (int a = 0; a < 360; a += 10) {
        float rad = a * 3.14159f / 180.0f;
        renderer.drawPixel(cx + (int)(s / 2 * cosf(rad)),
                           cy - s / 4 + (int)(s / 2 * sinf(rad)), true);
      }
      renderer.fillRect(cx - s / 4, cy + s / 4, s / 2, s / 3, true);
      break;
    }
    case 13: { // Wrench
      renderer.drawLine(cx - s, cy + s, cx + s / 2, cy - s / 2);
      renderer.drawLine(cx + s / 2, cy - s / 2, cx + s, cy - s / 3);
      renderer.drawLine(cx + s / 2, cy - s / 2, cx + s / 3, cy - s);
      break;
    }
    case 14: { // Magnifier
      for (int a = 0; a < 360; a += 10) {
        float rad = a * 3.14159f / 180.0f;
        renderer.drawPixel(cx - s / 4 + (int)(s / 2 * cosf(rad)),
                           cy - s / 4 + (int)(s / 2 * sinf(rad)), true);
      }
      renderer.drawLine(cx + s / 6,     cy + s / 6,     cx + s,     cy + s);
      renderer.drawLine(cx + s / 6 + 1, cy + s / 6,     cx + s + 1, cy + s);
      break;
    }
    case 15: { // Clock
      for (int a = 0; a < 360; a += 10) {
        float rad = a * 3.14159f / 180.0f;
        renderer.drawPixel(cx + (int)(s * cosf(rad)), cy + (int)(s * sinf(rad)), true);
      }
      renderer.drawLine(cx, cy, cx,          cy - s * 2 / 3);
      renderer.drawLine(cx, cy, cx + s / 2,  cy);
      break;
    }
    case 16: { // Envelope
      renderer.drawRect(cx - s, cy - s / 2, s * 2, s);
      renderer.drawLine(cx - s, cy - s / 2, cx, cy + s / 4);
      renderer.drawLine(cx + s, cy - s / 2, cx, cy + s / 4);
      break;
    }
    case 17: { // Star (5-pointed spokes)
      for (int i = 0; i < 5; i++) {
        float angle = -1.5708f + i * 1.2566f;
        int tx = cx + (int)(s * cosf(angle));
        int ty = cy + (int)(s * sinf(angle));
        renderer.drawLine(cx, cy, tx, ty);
      }
      break;
    }
    case 18: { // Moon
      for (int a = 0; a < 360; a += 5) {
        float rad = a * 3.14159f / 180.0f;
        renderer.drawPixel(cx + (int)(s * cosf(rad)), cy + (int)(s * sinf(rad)), true);
      }
      for (int dy2 = -s; dy2 <= s; dy2++) {
        for (int dx2 = -s; dx2 <= s; dx2++) {
          if (dx2 * dx2 + dy2 * dy2 <= (s - 2) * (s - 2)) {
            int ox = dx2 + s / 3;
            if (ox * ox + dy2 * dy2 <= s * s)
              renderer.drawPixel(cx + dx2 + s / 3, cy + dy2, false);
          }
        }
      }
      break;
    }
    case 19: { // Cloud
      for (int i = -1; i <= 1; i++) {
        for (int a = 0; a < 360; a += 10) {
          float rad = a * 3.14159f / 180.0f;
          int r2 = s / 2;
          renderer.drawPixel(cx + i * s / 3 + (int)(r2 * cosf(rad)),
                             cy + (int)(r2 * sinf(rad)), true);
        }
      }
      for (int a = 0; a < 360; a += 10) {
        float rad = a * 3.14159f / 180.0f;
        renderer.drawPixel(cx + (int)(s / 3 * cosf(rad)),
                           cy - s / 3 + (int)(s / 3 * sinf(rad)), true);
      }
      break;
    }
    case 20: { // Microchip
      renderer.drawRect(cx - s / 2, cy - s / 2, s, s);
      for (int i = 0; i < 4; i++) {
        int py = cy - s / 2 + 2 + i * (s - 4) / 3;
        renderer.fillRect(cx - s / 2 - s / 4, py, s / 4, 2, true);
        renderer.fillRect(cx + s / 2,          py, s / 4, 2, true);
      }
      renderer.fillRect(cx - s / 2 + 2, cy - s / 2 + 2, 3, 3, true);
      break;
    }
    case 21: { // Circuit Board
      renderer.drawRect(cx - s, cy - s, s * 2, s * 2);
      renderer.drawLine(cx - s / 2, cy - s, cx - s / 2, cy);
      renderer.drawLine(cx - s / 2, cy, cx + s / 2, cy);
      renderer.drawLine(cx + s / 2, cy, cx + s / 2, cy + s);
      renderer.fillRect(cx - s / 2 - 2, cy - 2, 4, 4, true);
      renderer.fillRect(cx + s / 2 - 2, cy + s - 2, 4, 4, true);
      break;
    }
    case 22: { // Router
      renderer.drawRect(cx - s, cy, s * 2, s / 2);
      renderer.drawLine(cx - s / 2, cy, cx - s / 3, cy - s);
      renderer.drawLine(cx + s / 2, cy, cx + s / 3, cy - s);
      renderer.fillRect(cx - s / 3 - 2, cy - s - 2, 4, 4, true);
      renderer.fillRect(cx + s / 3 - 2, cy - s - 2, 4, 4, true);
      for (int i = 0; i < 3; i++)
        renderer.fillRect(cx - s / 2 + i * s / 2, cy + s / 6, 3, 3, true);
      break;
    }
    case 23: { // Satellite
      renderer.drawRect(cx - s / 4, cy - s / 4, s / 2, s / 2);
      renderer.fillRect(cx - s,       cy - s / 6, s * 2 / 3, s / 3, true);
      renderer.fillRect(cx + s / 4,   cy - s / 6, s * 2 / 3, s / 3, true);
      renderer.drawLine(cx, cy - s / 4, cx, cy - s);
      renderer.fillRect(cx - 2, cy - s - 2, 4, 4, true);
      break;
    }
    case 24: { // Robot Head
      renderer.drawRect(cx - s, cy - s / 2, s * 2, s + s / 2);
      renderer.fillRect(cx - s / 2, cy - s / 4, s / 3, s / 3, true);
      renderer.fillRect(cx + s / 4,  cy - s / 4, s / 3, s / 3, true);
      renderer.drawLine(cx - s / 2, cy + s / 3, cx + s / 2, cy + s / 3);
      renderer.drawLine(cx, cy - s / 2, cx, cy - s);
      renderer.fillRect(cx - 2, cy - s - 3, 5, 3, true);
      break;
    }
    case 25: { // Game Boy
      renderer.drawRect(cx - s / 2, cy - s, s, s * 2);
      renderer.fillRect(cx - s / 3, cy - s + s / 4, s * 2 / 3, s / 2, false);
      renderer.drawRect(cx - s / 3, cy - s + s / 4, s * 2 / 3, s / 2);
      renderer.fillRect(cx + s / 6, cy + s / 3, 4, 4, true);
      renderer.fillRect(cx - s / 4, cy + s / 3, 4, 4, true);
      renderer.fillRect(cx - s / 3, cy + s / 2, s / 4, 2, true);
      renderer.fillRect(cx - s / 4, cy + s / 3, 2, s / 4, true);
      break;
    }
    case 26: { // Oscilloscope
      renderer.drawRect(cx - s, cy - s / 2, s * 2, s);
      for (int px = -s + 2; px < s - 2; px++) {
        float wave = sinf(px * 3.14159f / (float)(s / 2));
        int py = cy + (int)(s / 4 * wave);
        renderer.drawPixel(cx + px, py, true);
      }
      break;
    }
    case 27: { // Soldering Iron
      renderer.drawLine(cx - s, cy + s, cx + s / 3, cy - s / 3);
      renderer.drawLine(cx - s + 1, cy + s, cx + s / 3 + 1, cy - s / 3);
      renderer.drawLine(cx + s / 3, cy - s / 3, cx + s, cy - s);
      renderer.fillRect(cx + s - 2, cy - s - 2, 4, 4, true);
      break;
    }
    case 28: { // Drone
      renderer.drawLine(cx - s, cy - s, cx + s, cy + s);
      renderer.drawLine(cx + s, cy - s, cx - s, cy + s);
      renderer.fillRect(cx - 2, cy - 2, 4, 4, true);
      for (int corner = 0; corner < 4; corner++) {
        int dx2 = (corner & 1) ? s : -s;
        int dy2 = (corner & 2) ? s : -s;
        renderer.drawRect(cx + dx2 - 3, cy + dy2 - 3, 6, 6);
      }
      break;
    }
    case 29: { // VR Headset
      renderer.drawRect(cx - s, cy - s / 3, s * 2, s * 2 / 3);
      renderer.fillRect(cx - s / 2, cy - s / 6, s / 3, s / 3, true);
      renderer.fillRect(cx + s / 4,  cy - s / 6, s / 3, s / 3, true);
      renderer.drawLine(cx - s, cy, cx - s - s / 3, cy - s / 2);
      renderer.drawLine(cx + s, cy, cx + s + s / 3, cy - s / 2);
      break;
    }
    case 30: { // Server Rack
      for (int i = 0; i < 3; i++) {
        int ry = cy - s + i * (s * 2 / 3);
        renderer.drawRect(cx - s / 2, ry, s, s * 2 / 3 - 2);
        renderer.fillRect(cx + s / 3, ry + 3, 3, 3, true);
      }
      break;
    }
    case 31: { // Ethernet Jack
      renderer.drawRect(cx - s / 2, cy - s / 3, s, s * 2 / 3);
      renderer.drawRect(cx - s / 3, cy - s / 3 - s / 4, s * 2 / 3, s / 4);
      for (int i = 0; i < 4; i++)
        renderer.fillRect(cx - s / 4 + i * s / 6, cy - s / 3 - s / 6, 2, s / 6, true);
      break;
    }
    case 32: { // Raspberry Pi
      renderer.drawRect(cx - s, cy - s / 2, s * 2, s);
      renderer.fillRect(cx - s + 2, cy - s / 2 + 2, s / 3, s / 4, true);
      for (int i = 0; i < 5; i++)
        renderer.fillRect(cx + s / 2 + i * 3 - 7, cy - s / 2 - 3, 2, 3, true);
      renderer.fillRect(cx, cy - s / 4, s / 2, s / 3, false);
      renderer.drawRect(cx, cy - s / 4, s / 2, s / 3);
      break;
    }
    case 33: { // Arduino
      renderer.drawRect(cx - s, cy - s / 2, s * 2, s);
      renderer.fillRect(cx - s / 2, cy - s / 2 - 2, s, 2, true);
      renderer.fillRect(cx - s / 4, cy + s / 2, s / 2, 2, true);
      renderer.fillRect(cx + s / 3, cy - s / 4, s / 3, s / 4, true);
      break;
    }
    case 34: { // Logic Analyzer
      renderer.drawRect(cx - s / 2, cy - s / 3, s, s * 2 / 3);
      for (int i = 0; i < 4; i++) {
        int px = cx - s / 3 + i * s / 4;
        renderer.drawLine(px, cy - s / 3, px, cy - s);
        renderer.fillRect(px - 1, cy - s - 2, 3, 3, true);
      }
      break;
    }
    case 35: { // Golden Chip
      lbFillDithered25(renderer, cx - s, cy - s, s * 2, s * 2);
      renderer.drawRect(cx - s, cy - s, s * 2, s * 2);
      renderer.drawRect(cx - s + 2, cy - s + 2, s * 2 - 4, s * 2 - 4);
      for (int i = 0; i < 3; i++) {
        int py = cy - s + 4 + i * (s * 2 - 8) / 2;
        renderer.fillRect(cx - s - s / 3, py, s / 3, 3, true);
        renderer.fillRect(cx + s,          py, s / 3, 3, true);
      }
      break;
    }
    case 36: { // Cyber Eye
      for (int a = 0; a < 360; a += 5) {
        float rad = a * 3.14159f / 180.0f;
        renderer.drawPixel(cx + (int)(s * cosf(rad)), cy + (int)(s * sinf(rad)), true);
      }
      for (int dy2 = -s / 3; dy2 <= s / 3; dy2++) {
        int dx2 = 0;
        while ((dx2 + 1) * (dx2 + 1) + dy2 * dy2 <= (s / 3) * (s / 3)) dx2++;
        if (dx2 > 0) renderer.fillRect(cx - dx2, cy + dy2, dx2 * 2 + 1, 1, true);
      }
      renderer.drawLine(cx - s - 3, cy, cx - s, cy);
      renderer.drawLine(cx + s, cy, cx + s + 3, cy);
      renderer.drawLine(cx, cy - s, cx, cy - s - 3);
      break;
    }
    case 37: { // Plasma Ball
      lbFillDithered25(renderer, cx - s, cy - s, s * 2, s * 2);
      for (int a = 0; a < 360; a += 5) {
        float rad = a * 3.14159f / 180.0f;
        renderer.drawPixel(cx + (int)(s * cosf(rad)), cy + (int)(s * sinf(rad)), true);
      }
      renderer.drawLine(cx, cy, cx + s / 2, cy - s / 3);
      renderer.drawLine(cx + s / 2, cy - s / 3, cx + s / 4, cy - s / 6);
      renderer.drawLine(cx + s / 4, cy - s / 6, cx + s * 2 / 3, cy - s * 2 / 3);
      renderer.drawLine(cx, cy, cx - s / 3, cy + s / 2);
      renderer.drawLine(cx - s / 3, cy + s / 2, cx - s / 6, cy + s / 3);
      break;
    }
    case 38: { // Hologram
      renderer.drawLine(cx,      cy - s, cx + s,  cy);
      renderer.drawLine(cx + s,  cy,     cx,      cy + s);
      renderer.drawLine(cx,      cy + s, cx - s,  cy);
      renderer.drawLine(cx - s,  cy,     cx,      cy - s);
      lbFillDithered25(renderer, cx - s / 2, cy - s / 2, s, s);
      break;
    }
    case 39: { // Quantum Bit
      for (int a = 0; a < 360; a += 8) {
        float rad = a * 3.14159f / 180.0f;
        int r2 = s * 2 / 3;
        renderer.drawPixel(cx - s / 4 + (int)(r2 * cosf(rad)), cy + (int)(r2 * sinf(rad)), true);
        renderer.drawPixel(cx + s / 4 + (int)(r2 * cosf(rad)), cy + (int)(r2 * sinf(rad)), true);
      }
      lbFillDithered50(renderer, cx - s / 6, cy - s / 3, s / 3, s * 2 / 3);
      break;
    }
    case 40: { // Neural Net
      int nodes[5][2] = {{cx, cy - s}, {cx - s, cy}, {cx + s, cy}, {cx - s / 2, cy + s}, {cx + s / 2, cy + s}};
      for (int i = 0; i < 5; i++) {
        renderer.fillRect(nodes[i][0] - 2, nodes[i][1] - 2, 5, 5, true);
        for (int j = i + 1; j < 5; j++)
          renderer.drawLine(nodes[i][0], nodes[i][1], nodes[j][0], nodes[j][1]);
      }
      break;
    }
    case 41: { // Infinity Loop
      for (int a = 0; a < 360; a += 5) {
        float rad = a * 3.14159f / 180.0f;
        float ix = s * 2 / 3 * cosf(rad);
        float iy = s / 2 * sinf(rad) * cosf(rad);
        renderer.drawPixel(cx + (int)ix, cy + (int)iy, true);
      }
      break;
    }
    case 42: { // Crypto Key
      renderer.drawRect(cx - s, cy - s / 4, s, s / 2);
      renderer.drawLine(cx, cy, cx + s, cy);
      renderer.drawLine(cx + s / 2,     cy, cx + s / 2,     cy + s / 3);
      renderer.drawLine(cx + s * 3 / 4, cy, cx + s * 3 / 4, cy + s / 4);
      break;
    }
    case 43: { // Zero Day
      lbFillDithered50(renderer, cx - s / 2, cy - s / 2, s, s);
      renderer.drawRect(cx - s / 2, cy - s / 2, s, s);
      renderer.fillRect(cx - s / 3, cy - s / 4, s / 4, s / 4, false);
      renderer.fillRect(cx + s / 8, cy - s / 4, s / 4, s / 4, false);
      renderer.fillRect(cx - s / 6, cy + s / 6, s / 3, 2, false);
      break;
    }
    case 44: { // Black Box
      renderer.fillRect(cx - s, cy - s, s * 2, s * 2, true);
      lbFillDithered50(renderer, cx - s + 3, cy - s + 3, s * 2 - 6, s * 2 - 6);
      renderer.drawRect(cx - s / 3, cy - s / 3, s * 2 / 3, s * 2 / 3);
      break;
    }
    case 45: { // The Kernel
      renderer.fillRect(cx - s - 2, cy - s - 2, s * 2 + 4, s * 2 + 4, true);
      renderer.fillRect(cx - s, cy - s + s / 3, s * 2, s * 2 - s / 3, false);
      renderer.fillRect(cx - s, cy - s, s * 2, s / 3, true);
      renderer.fillRect(cx + s - s / 4, cy - s + 2, s / 5, s / 5, false);
      for (int i = 0; i < 5; i++)
        renderer.fillRect(cx - s + 4 + i * 4, cy - s / 4, 2, 3, true);
      renderer.fillRect(cx - s + 24, cy - s / 4, 3, 5, true);
      break;
    }
    case 46: { // Root Shell
      renderer.drawRect(cx - s, cy - s, s * 2, s * 2);
      renderer.fillRect(cx - s, cy - s, s * 2, s / 3, true);
      renderer.fillRect(cx + s - s / 4, cy - s + 2, s / 5, s / 5, false);
      renderer.drawLine(cx - s / 2,       cy - s / 6, cx - s / 2 + s / 3, cy - s / 6);
      renderer.drawLine(cx - s / 2,       cy + s / 6, cx - s / 2 + s / 3, cy + s / 6);
      renderer.drawLine(cx - s / 3, cy - s / 3, cx - s / 3, cy + s / 3);
      renderer.drawLine(cx - s / 6, cy - s / 3, cx - s / 6, cy + s / 3);
      break;
    }
    case 47: { // Packet Ghost
      for (int a = 0; a <= 180; a += 5) {
        float rad = a * 3.14159f / 180.0f;
        renderer.drawPixel(cx + (int)(s * 2 / 3 * cosf(rad)),
                           cy - s / 4 + (int)(s * 2 / 3 * -sinf(rad)), true);
      }
      renderer.drawLine(cx - s * 2 / 3, cy - s / 4, cx - s * 2 / 3, cy + s);
      renderer.drawLine(cx + s * 2 / 3, cy - s / 4, cx + s * 2 / 3, cy + s);
      renderer.drawLine(cx - s * 2 / 3, cy + s, cx - s / 3, cy + s / 2);
      renderer.drawLine(cx - s / 3,     cy + s / 2, cx,          cy + s);
      renderer.drawLine(cx,             cy + s,     cx + s / 3,  cy + s / 2);
      renderer.drawLine(cx + s / 3,     cy + s / 2, cx + s * 2 / 3, cy + s);
      renderer.fillRect(cx - s / 3, cy - s / 3, 3, 3, true);
      renderer.fillRect(cx + s / 4, cy - s / 3, 3, 3, true);
      break;
    }
    case 48: { // E-Ink Dragon
      for (int dy2 = -s / 2; dy2 <= s / 2; dy2++) {
        float ratio = 1.0f - (float)(dy2 * dy2) / ((float)(s / 2) * (s / 2));
        if (ratio < 0.0f) ratio = 0.0f;
        int hw = (int)(s * sqrtf(ratio));
        if (hw > 0) renderer.fillRect(cx - hw / 2, cy + dy2, hw, 1, true);
      }
      renderer.drawLine(cx - s / 2, cy - s / 4, cx - s, cy - s);
      renderer.drawLine(cx - s,     cy - s,      cx - s / 3, cy);
      renderer.drawLine(cx + s / 2, cy - s / 4, cx + s, cy - s);
      renderer.drawLine(cx + s,     cy - s,      cx + s / 3, cy);
      renderer.drawLine(cx + s / 2, cy - s / 4, cx + s, cy);
      renderer.drawLine(cx + s / 2, cy + s / 4, cx + s, cy);
      renderer.drawLine(cx - s / 2, cy,           cx - s, cy + s / 2);
      renderer.drawLine(cx - s,     cy + s / 2,   cx - s + s / 3, cy + s);
      break;
    }
    case 49: { // biscuit. Logo
      for (int a = 0; a < 360; a += 5) {
        float rad = a * 3.14159f / 180.0f;
        renderer.drawPixel(cx + (int)(s * cosf(rad)),       cy + (int)(s * sinf(rad)), true);
        renderer.drawPixel(cx + (int)((s - 2) * cosf(rad)), cy + (int)((s - 2) * sinf(rad)), true);
      }
      renderer.fillRect(cx - s / 3, cy - s / 2, 2, s, true);
      renderer.drawRect(cx - s / 3, cy, s / 2, s / 3);
      renderer.fillRect(cx + s / 4, cy + s / 4, 3, 3, true);
      break;
    }
    default: {
      renderer.drawRect(x + 2, y + 2, size - 4, size - 4);
      char buf[4];
      snprintf(buf, sizeof(buf), "%d", iconId % 100);
      int tw = renderer.getTextWidth(SMALL_FONT_ID, buf);
      renderer.drawText(SMALL_FONT_ID, cx - tw / 2, cy - 5, buf);
      break;
    }
  }
}
