#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class CasinoActivity final : public Activity {
 public:
  explicit CasinoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Casino", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  // ---- top-level state ----
  enum Screen { LOBBY, SLOTS, BLACKJACK, COINFLIP, HIGHLOW, ROULETTE, LOOTBOX };
  Screen screen = LOBBY;
  int lobbyIndex = 0;
  int resetConfirmCount = 0;
  static constexpr int LOBBY_COUNT = 7;  // Slots, BJ, Coin, H/L, Roulette, Loot Box, Reset
  ButtonNavigator buttonNavigator;

  // ---- credits ----
  int32_t credits = 1000;
  static constexpr const char* SAVE_PATH = "/biscuit/casino.dat";
  void loadCredits();
  void saveCredits();

  // ---- shared ----
  static constexpr int BET_OPTIONS[] = {10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000, 25000, 50000, 100000};
  static constexpr int NUM_BETS = 13;
  int betIndex = 1;  // default 25
  int currentBet() const { return BET_OPTIONS[betIndex]; }
  std::string resultMessage;
  int resultAmount = 0;  // positive=win, negative=loss
  bool showingResult = false;

  // ============ SLOTS (overhauled) ============
  struct SlotSymbolSet {
    uint8_t ids[8];
    int8_t payouts[8];  // 3-match multiplier per symbol
  };

  struct SlotMachineType {
    const char* name;
    const char* description;
    uint8_t numSymbols;
    SlotSymbolSet symbols;
    int8_t twoMatchMult;   // payout for 2-of-a-kind (0 = none)
    uint8_t wildSymbolIdx; // 0xFF = no wild
    bool hasFreeSpin;
    bool hasHoldReel;
    int16_t minBet;
  };

  static constexpr int NUM_MACHINES = 5;
  static const SlotMachineType MACHINES[NUM_MACHINES];

  enum SlotsScreen { SLOTS_MENU, SLOTS_PLAY, SLOTS_PAYOUTS, SLOTS_POWERUPS };
  enum SlotsPlayState { SP_BET, SP_HOLD_SELECT, SP_SPIN, SP_RESULT };

  SlotsScreen slotsScreen = SLOTS_MENU;
  SlotsPlayState slotsPlayState = SP_BET;
  int slotsMachineIndex = 0;
  int slotsMachineMenuIndex = 0;

  int reels[3] = {0, 0, 0};
  bool reelHold[3] = {false, false, false};
  int holdCursor = 0;

  int animFrame = 0;
  unsigned long animStartMs = 0;
  static constexpr int SPIN_FRAMES = 12;
  static constexpr unsigned long SPIN_FRAME_MS = 120;
  int reelStopFrame[3] = {8, 10, 12};  // when each reel locks to final value
  int finalReels[3] = {0, 0, 0};
  bool slotsShowLastResult = false;

  struct SlotsPowerups {
    uint8_t freeSpins = 0;
    uint8_t multiplier = 1;
    bool wildActive = false;
  };
  SlotsPowerups slotsPowerups;
  int powerupMenuIndex = 0;

  void slotsLoop();
  void slotsMenuLoop();
  void slotsPlayLoop();
  void slotsPayoutsLoop();
  void slotsPowerupsLoop();
  void slotsSpin();
  void slotsEvaluate();

  void slotsRender();
  void slotsRenderMenu();
  void slotsRenderPlay();
  void slotsRenderPayouts();
  void slotsRenderPowerups();
  void drawSlotReel(int x, int y, int symbolId, bool held, bool blur, int blurFrame);
  void drawReelBlur(int x, int y, int w, int h, int frameOffset);
  void drawSlotIcon(int cx, int cy, uint8_t symbolId);

  // ============ BLACKJACK ============
  enum BJState { BJ_BET, BJ_PLAYING, BJ_DEALER, BJ_RESULT };
  BJState bjState = BJ_BET;

  struct Card { uint8_t rank; uint8_t suit; };  // rank 1-13, suit 0-3
  static constexpr const char* SUIT_CHARS[] = {"S", "H", "D", "C"};
  static constexpr const char* RANK_CHARS[] = {"", "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K"};

  Card deck[52];
  int deckPos = 0;
  std::vector<Card> playerHand;
  std::vector<Card> dealerHand;
  bool dealerRevealed = false;

  void bjShuffle();
  Card bjDraw();
  int bjHandValue(const std::vector<Card>& hand) const;
  bool bjIsBlackjack(const std::vector<Card>& hand) const;
  std::string bjCardStr(const Card& c) const;
  void bjDeal();
  void bjDealerPlay();
  void bjEvaluate();
  void bjLoop();
  void bjRender();

  // ============ COIN FLIP ============
  enum CoinState { COIN_BET, COIN_PICK, COIN_FLIPPING, COIN_RESULT };
  CoinState coinState = COIN_BET;
  int coinPick = 0;  // 0=heads, 1=tails
  int coinResult = 0;
  int coinAnimFrame = 0;
  unsigned long coinAnimStartMs = 0;
  static constexpr int COIN_ANIM_FRAMES = 5;
  static constexpr unsigned long COIN_FRAME_MS = 200;

  void coinLoop();
  void coinFlip();
  void coinRender();

  // ============ HIGHER / LOWER ============
  enum HLState { HL_BET, HL_PLAYING, HL_RESULT };
  HLState hlState = HL_BET;
  Card hlCurrentCard;
  Card hlNextCard;
  int hlStreak = 0;
  int hlPot = 0;

  void hlLoop();
  void hlDraw();
  void hlGuess(bool higher);
  void hlCashOut();
  void hlRender();

  // ============ ROULETTE ============
  enum RLState { RL_BET, RL_PICK, RL_RESULT };
  RLState rlState = RL_BET;

  enum RLBetType { RL_RED, RL_BLACK, RL_ODD, RL_EVEN, RL_LOW, RL_HIGH, RL_DOZ1, RL_DOZ2, RL_DOZ3, RL_NUMBER };
  static constexpr int RL_NUM_BET_TYPES = 10;
  RLBetType rlBetType = RL_RED;
  int rlNumber = 1;  // 0-36 for straight bet
  int rlResult = 0;  // winning number 0-36

  static bool isRed(int n);
  static const char* rlBetName(RLBetType t, int num, char* buf, int bufsz);
  bool rlCellHighlighted(int n) const;
  void rlLoop();
  void rlSpin();
  void rlRender();

  void resetCredits() { credits = 1000; saveCredits(); }

  // ---- rendering helpers ----
  void renderLobby();
  void renderCreditsBar();
  void renderBetSelector(int y);
  void drawCard(int x, int y, const Card& c, bool faceDown = false);

  // ============ LOOT BOX ============
  static constexpr const char* LB_COLLECTION_SAVE_PATH = "/biscuit/lootbox.dat";

  // Collection: 50 items stored as bitfield (7 bytes = 56 bits, using first 50)
  uint8_t lbCollected[7] = {};
  int lbTotalCollected = 0;

  // Item database
  enum LbRarity : uint8_t { LB_COMMON, LB_RARE, LB_EPIC, LB_LEGENDARY };
  struct LbItem {
    const char* name;
    LbRarity rarity;
    uint8_t iconId;
  };
  static const LbItem LB_ITEMS[50];
  static constexpr int LB_ITEM_COUNT = 50;
  static constexpr int LB_COMMON_COUNT = 20;
  static constexpr int LB_RARE_COUNT = 15;
  static constexpr int LB_EPIC_COUNT = 10;
  static constexpr int LB_LEGENDARY_COUNT = 5;

  // Pull costs
  static constexpr int LB_SINGLE_COST = 100;
  static constexpr int LB_MULTI_COST = 450;
  static constexpr int LB_MULTI_COUNT = 5;

  // Pull results
  int lbPullResults[5] = {};
  int lbPullCount = 0;
  bool lbPullIsNew[5] = {};
  int lbRevealIndex = 0;

  // Animation state
  enum LbAnimState { LB_ANIM_IDLE, LB_ANIM_SHAKING, LB_ANIM_OPENING, LB_ANIM_REVEALED };
  LbAnimState lbAnimState = LB_ANIM_IDLE;
  int lbAnimFrame = 0;
  unsigned long lbAnimStartMs = 0;
  static constexpr int LB_SHAKE_FRAMES = 4;
  static constexpr unsigned long LB_SHAKE_FRAME_MS = 150;
  static constexpr int LB_OPEN_FRAMES = 3;
  static constexpr unsigned long LB_OPEN_FRAME_MS = 250;

  // Loot box sub-screen
  enum LbScreen { LB_MAIN_MENU, LB_PULLING, LB_REVEAL_SINGLE, LB_REVEAL_MULTI, LB_COLLECTION, LB_ITEM_DETAIL };
  LbScreen lbScreen = LB_MAIN_MENU;
  int lbMenuIndex = 0;

  // Collection browser
  int lbCollectionPage = 0;
  int lbCollectionCursor = 0;
  static constexpr int LB_ITEMS_PER_PAGE = 15;
  static constexpr int LB_GRID_COLS = 5;
  static constexpr int LB_GRID_ROWS = 3;

  // Detail view
  int lbDetailItemId = -1;

  // Collection helpers
  void lbLoadCollection();
  void lbSaveCollection();
  bool lbHasItem(int id) const;
  void lbSetItem(int id);
  int lbCountCollected() const;

  // Gacha logic
  LbRarity lbRollRarity(bool guaranteeRare = false);
  int lbRollItem(bool guaranteeRare = false);
  void lbPerformSinglePull();
  void lbPerformMultiPull();

  // Loot box loop/render
  void lbLoop();
  void lbRender();

  // Render helpers
  void lbRenderMainMenu();
  void lbRenderPulling();
  void lbRenderRevealSingle();
  void lbRenderRevealMulti();
  void lbRenderCollection();
  void lbRenderItemDetail();

  // Drawing helpers
  void lbDrawItemIcon(int x, int y, int size, int iconId, bool locked) const;
  void lbDrawLootBox(int cx, int cy, int size, int shakeOffset) const;
  void lbDrawRarityBorder(int x, int y, int w, int h, LbRarity rarity) const;
  void lbDrawStars(int cx, int y, LbRarity rarity) const;
  static const char* lbRarityName(LbRarity r);

  // Dithering helpers (static, work on const renderer reference)
  static void lbFillDithered25(const GfxRenderer& r, int x, int y, int w, int h);
  static void lbFillDithered50(const GfxRenderer& r, int x, int y, int w, int h);
  static void lbFillDithered75(const GfxRenderer& r, int x, int y, int w, int h);
};
