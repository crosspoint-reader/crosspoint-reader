#pragma once
#include <string>
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct RadarNode;
struct RadarHomeStatus;

class AppsMenuActivity final : public Activity {
 public:
  explicit AppsMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("AppsMenu", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  int selectorIndex = 0;
  static constexpr int ITEM_COUNT = 8;
  static constexpr int COLS = 2;
  static constexpr int ROWS = 4;

  // Grid navigation
  int getRow() const { return selectorIndex / COLS; }
  int getCol() const { return selectorIndex % COLS; }

  // Cached system info (refreshed on enter + periodically)
  uint32_t freeHeap = 0;
  uint8_t batteryPercent = 0;
  unsigned long uptimeSeconds = 0;
  bool wifiConnected = false;
  unsigned long lastInfoRefresh = 0;
  static constexpr unsigned long INFO_REFRESH_MS = 30000;
  char uptimeStr[16] = "";

  // Badge counts (refreshed with system info)
  int badgeRecon = 0;       // tracker alerts count
  int badgeSecurity = -1;   // 0 = ok, -1 = PIN not set (show "!")
  int badgeSystem = 0;      // firmware update available

  void refreshSystemInfo();

  // Last-used activity per category (read from SD on enter)
  char lastUsedName[ITEM_COUNT][32] = {};
  void loadLastUsed();

  // Tile rendering
  void drawTile(int index, int x, int y, int w, int h, bool selected) const;
  void drawStatusBar() const;

};
