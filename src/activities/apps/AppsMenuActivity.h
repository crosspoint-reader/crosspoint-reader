#pragma once
#include <string>
#include <vector>
#include "RecentBooksStore.h"
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
  static constexpr int ITEM_COUNT = 4;
  static constexpr int COLS = 2;
  static constexpr int TILE_ROWS = 2;
  static constexpr int MAX_RECENT = 3;

  // Grid navigation helpers (tile zone only)
  int getTileRow() const { return selectorIndex / COLS; }
  int getTileCol() const { return selectorIndex % COLS; }
  bool isRecentSelected() const { return selectorIndex >= ITEM_COUNT; }
  int recentIdx() const { return selectorIndex - ITEM_COUNT; }

  // Recent books (loaded on enter, up to MAX_RECENT)
  std::vector<RecentBook> recentBooks;
  void loadRecentBooks();

  // Cached system info (refreshed on enter + periodically)
  uint32_t freeHeap = 0;
  uint8_t batteryPercent = 0;
  unsigned long uptimeSeconds = 0;
  bool wifiConnected = false;
  int8_t wifiRssi = 0;
  unsigned long lastInfoRefresh = 0;
  static constexpr unsigned long INFO_REFRESH_MS = 30000;
  char uptimeStr[16] = "";

  void refreshSystemInfo();

  // Last-used activity per category (read from SD on enter)
  char lastUsedName[ITEM_COUNT][32] = {};
  void loadLastUsed();

  // Rendering
  void drawTile(int index, int x, int y, int w, int h, bool selected) const;
  void drawStatusBar() const;
  void drawRecentBooks(int x, int y, int w, int h) const;
};
