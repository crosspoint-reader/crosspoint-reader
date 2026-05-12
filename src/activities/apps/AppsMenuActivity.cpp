#include "AppsMenuActivity.h"

#include <I18n.h>

#include "AppCategoryActivity.h"
#include "BleScannerActivity.h"
#include "CasinoActivity.h"
#include "ChessActivity.h"
#include "DiceRollerActivity.h"
#include "GameOfLifeActivity.h"
#include "MappedInputManager.h"
#include "MinesweeperActivity.h"
#include "ClockActivity.h"
#include "PasswordManagerActivity.h"
#include "SnakeActivity.h"
#include "SudokuActivity.h"
#include "TetrisActivity.h"
#include "SdFileBrowserActivity.h"
#include "UnitConverterActivity.h"
#include "VoronoiActivity.h"
#include "WifiConnectActivity.h"
#include "WifiScannerActivity.h"
#include "MatrixRainActivity.h"
#include "MazeActivity.h"
#include "CalculatorActivity.h"
#include "TaskManagerActivity.h"
#include "BatteryMonitorActivity.h"
#include "DeviceInfoActivity.h"
#include "BackgroundManagerActivity.h"
#include "ReadingStatsActivity.h"
#include "components/themes/radar/RadarHomeRenderer.h"
#include "activities/home/FileBrowserActivity.h"
#include "activities/home/RecentBooksActivity.h"
#include "activities/browser/OpdsBookBrowserActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/network/NetworkModeSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <WiFi.h>
#include <HalPowerManager.h>
#include <HalStorage.h>

// Radar home node table — kept in flash (.rodata) as constexpr.
static constexpr RadarNode kRadarNodes[4] = {
  {"APPS",     9},
  {"GAMES",   11},
  {"READER",   5},
  {"SETTINGS", 4},
};

void AppsMenuActivity::onEnter() {
  Activity::onEnter();
  selectorIndex = 0;
  // Check badges once on enter (SD I/O only here, not in periodic refresh)
  refreshSystemInfo();
  loadLastUsed();
  requestUpdate();
}

void AppsMenuActivity::loop() {
  // === RADAR MODE: circular navigation ===
  if (SETTINGS.uiTheme == CrossPointSettings::UI_THEME::RADAR) {
    // Right/Down advances clockwise; Left/Up goes counter-clockwise.
    if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      selectorIndex = (selectorIndex + 1) % ITEM_COUNT;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
               mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      selectorIndex = (selectorIndex - 1 + ITEM_COUNT) % ITEM_COUNT;
      requestUpdate();
    }
    // Periodic info refresh in radar mode
    if (millis() - lastInfoRefresh > INFO_REFRESH_MS) {
      uint32_t oldHeap = freeHeap;
      bool oldWifi = wifiConnected;
      refreshSystemInfo();
      bool heapChanged = (freeHeap / 1024) != (oldHeap / 1024);
      if (heapChanged || (wifiConnected != oldWifi)) {
        requestUpdate();
      }
    }
    // Confirm and Back use the same switch as the grid — fall through to shared confirm block below.
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      std::unique_ptr<Activity> app;
      switch (selectorIndex) {
          case 0: {
            std::vector<AppCategoryActivity::AppEntry> e = {
                {tr(STR_WIFI_CONNECT), "Scan, connect, saved networks", UIIcon::Wifi, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<WifiConnectActivity>(r, m); }},
                {"Bluetooth", "Scan nearby BLE devices", UIIcon::Hotspot, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<BleScannerActivity>(r, m); }},
                {tr(STR_WIFI_SCANNER), "APs, signal, channels", UIIcon::Wifi, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<WifiScannerActivity>(r, m); }},
                {"Calculator", "Basic calculator", UIIcon::File, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<CalculatorActivity>(r, m); }},
                {"Clock", "NTP clock / stopwatch / pomodoro", UIIcon::Recent, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<ClockActivity>(r, m); }},
                {tr(STR_UNIT_CONVERTER), "Convert between units", UIIcon::File, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<UnitConverterActivity>(r, m); }},
                {"File Browser", "Browse files on SD card", UIIcon::Folder, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<SdFileBrowserActivity>(r, m); }},
                {"WiFi Transfer", "Upload/download via WiFi", UIIcon::Transfer, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<NetworkModeSelectionActivity>(r, m); }},
            };
            app = std::make_unique<AppCategoryActivity>(renderer, mappedInput, "Apps", std::move(e), false, 0);
            break;
          }
          case 1: {
            std::vector<AppCategoryActivity::AppEntry> e = {
                {"Casino", "Slots, blackjack, roulette + lootbox", UIIcon::File, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<CasinoActivity>(r, m); }, false, []() -> bool { return Storage.exists("/biscuit/casino.dat"); }},
                {tr(STR_MINESWEEPER), "Classic minesweeper", UIIcon::File, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<MinesweeperActivity>(r, m); }},
                {tr(STR_SUDOKU), "Number puzzle", UIIcon::File, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<SudokuActivity>(r, m); }},
                {tr(STR_CHESS), "Play against the device", UIIcon::File, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<ChessActivity>(r, m); }},
                {tr(STR_SNAKE), "Classic snake game", UIIcon::File, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<SnakeActivity>(r, m); }},
                {tr(STR_TETRIS), "Block stacking", UIIcon::File, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<TetrisActivity>(r, m); }},
                {"Maze", "Navigate random mazes", UIIcon::File, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<MazeActivity>(r, m); }},
                {tr(STR_DICE_ROLLER), "Roll dice with animation", UIIcon::File, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<DiceRollerActivity>(r, m); }},
                {tr(STR_GAME_OF_LIFE), "Conway's cellular automaton", UIIcon::File, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<GameOfLifeActivity>(r, m); }},
                {tr(STR_VORONOI), "Generate Voronoi patterns", UIIcon::File, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<VoronoiActivity>(r, m); }},
                {"Matrix Rain", "The Matrix digital rain effect", UIIcon::File, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<MatrixRainActivity>(r, m); }},
            };
            app = std::make_unique<AppCategoryActivity>(renderer, mappedInput, tr(STR_GAMES), std::move(e), false, 1);
            break;
          }
          case 2: {
            std::vector<AppCategoryActivity::AppEntry> e = {
                {"Open Book", "Browse and open an ebook", UIIcon::Book, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<FileBrowserActivity>(r, m); }},
                {"Recent Books", "Continue where you left off", UIIcon::Recent, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<RecentBooksActivity>(r, m); }},
                {"OPDS Browser", "Download books from OPDS servers", UIIcon::Library, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<OpdsBookBrowserActivity>(r, m); }},
                {"Reading Stats", "Pages read, streaks, progress", UIIcon::Book, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<ReadingStatsActivity>(r, m); }},
            };
            app = std::make_unique<AppCategoryActivity>(renderer, mappedInput, "Reader", std::move(e), false, 2);
            break;
          }
          case 3: {
            app = std::make_unique<SettingsActivity>(renderer, mappedInput);
            break;
          }
        }
      if (app) activityManager.pushActivity(std::move(app));
    }
    // Back button ignored on main screen — use Power button to sleep
    return;
  }

  // === 2D GRID NAVIGATION ===
  // Left/Right (front buttons) move between columns
  // Up/Down (side volume buttons) move between rows

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    int col = getCol();
    int row = getRow();
    col++;
    if (col >= COLS) {
      col = 0;
      row = (row + 1) % ROWS;
    }
    selectorIndex = row * COLS + col;
    if (selectorIndex >= ITEM_COUNT) selectorIndex = 0;
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    int col = getCol();
    int row = getRow();
    col--;
    if (col < 0) {
      col = COLS - 1;
      row = (row - 1 + ROWS) % ROWS;
    }
    selectorIndex = row * COLS + col;
    if (selectorIndex >= ITEM_COUNT) selectorIndex = ITEM_COUNT - 1;
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    int col = getCol();
    int row = (getRow() + 1) % ROWS;
    selectorIndex = row * COLS + col;
    if (selectorIndex >= ITEM_COUNT) selectorIndex = col;
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    int col = getCol();
    int row = (getRow() - 1 + ROWS) % ROWS;
    selectorIndex = row * COLS + col;
    if (selectorIndex >= ITEM_COUNT) {
      row = (row - 1 + ROWS) % ROWS;
      selectorIndex = row * COLS + col;
    }
    requestUpdate();
  }

  // Periodic info refresh — only redraw if visible values changed
  if (millis() - lastInfoRefresh > INFO_REFRESH_MS) {
    uint32_t oldHeap = freeHeap;
    bool oldWifi = wifiConnected;
    refreshSystemInfo();
    // Only trigger e-ink refresh if KB-level heap changed or wifi status changed
    bool heapChanged = (freeHeap / 1024) != (oldHeap / 1024);
    if (heapChanged || (wifiConnected != oldWifi)) {
      requestUpdate();
    }
  }

  // === CONFIRM: open category ===
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    std::unique_ptr<Activity> app;
    switch (selectorIndex) {
        case 0: {
          std::vector<AppCategoryActivity::AppEntry> e = {
              {tr(STR_WIFI_CONNECT), "Scan, connect, saved networks", UIIcon::Wifi, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<WifiConnectActivity>(r, m); }},
              {"Bluetooth", "Scan nearby BLE devices", UIIcon::Hotspot, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<BleScannerActivity>(r, m); }},
              {tr(STR_WIFI_SCANNER), "APs, signal, channels", UIIcon::Wifi, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<WifiScannerActivity>(r, m); }},
              {"Calculator", "Basic calculator", UIIcon::File, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<CalculatorActivity>(r, m); }},
              {"Clock", "NTP clock / stopwatch / pomodoro", UIIcon::Recent, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<ClockActivity>(r, m); }},
              {tr(STR_UNIT_CONVERTER), "Convert between units", UIIcon::File, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<UnitConverterActivity>(r, m); }},
              {"File Browser", "Browse files on SD card", UIIcon::Folder, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<SdFileBrowserActivity>(r, m); }},
              {"WiFi Transfer", "Upload/download via WiFi", UIIcon::Transfer, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<NetworkModeSelectionActivity>(r, m); }},
          };
          app = std::make_unique<AppCategoryActivity>(renderer, mappedInput, "Apps", std::move(e), false, 0);
          break;
        }
        case 1: {
          std::vector<AppCategoryActivity::AppEntry> e = {
              {"Casino", "Slots, blackjack, roulette + lootbox", UIIcon::File, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<CasinoActivity>(r, m); }, false, []() -> bool { return Storage.exists("/biscuit/casino.dat"); }},
              {tr(STR_MINESWEEPER), "Classic minesweeper", UIIcon::File, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<MinesweeperActivity>(r, m); }},
              {tr(STR_SUDOKU), "Number puzzle", UIIcon::File, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<SudokuActivity>(r, m); }},
              {tr(STR_CHESS), "Play against the device", UIIcon::File, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<ChessActivity>(r, m); }},
              {tr(STR_SNAKE), "Classic snake game", UIIcon::File, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<SnakeActivity>(r, m); }},
              {tr(STR_TETRIS), "Block stacking", UIIcon::File, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<TetrisActivity>(r, m); }},
              {"Maze", "Navigate random mazes", UIIcon::File, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<MazeActivity>(r, m); }},
              {tr(STR_DICE_ROLLER), "Roll dice with animation", UIIcon::File, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<DiceRollerActivity>(r, m); }},
              {tr(STR_GAME_OF_LIFE), "Conway's cellular automaton", UIIcon::File, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<GameOfLifeActivity>(r, m); }},
              {tr(STR_VORONOI), "Generate Voronoi patterns", UIIcon::File, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<VoronoiActivity>(r, m); }},
              {"Matrix Rain", "The Matrix digital rain effect", UIIcon::File, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<MatrixRainActivity>(r, m); }},
          };
          app = std::make_unique<AppCategoryActivity>(renderer, mappedInput, tr(STR_GAMES), std::move(e), false, 1);
          break;
        }
        case 2: {
          std::vector<AppCategoryActivity::AppEntry> e = {
              {"Open Book", "Browse and open an ebook", UIIcon::Book, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<FileBrowserActivity>(r, m); }},
              {"Recent Books", "Continue where you left off", UIIcon::Recent, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<RecentBooksActivity>(r, m); }},
              {"OPDS Browser", "Download books from OPDS servers", UIIcon::Library, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<OpdsBookBrowserActivity>(r, m); }},
              {"Reading Stats", "Pages read, streaks, progress", UIIcon::Book, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<ReadingStatsActivity>(r, m); }},
          };
          app = std::make_unique<AppCategoryActivity>(renderer, mappedInput, "Reader", std::move(e), false, 2);
          break;
        }
        case 3: {
          app = std::make_unique<SettingsActivity>(renderer, mappedInput);
          break;
        }
      }
    if (app) activityManager.pushActivity(std::move(app));
  }

  // Back button ignored on main screen — use Power button to sleep
}

void AppsMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  // === RADAR MODE ===
  if (SETTINGS.uiTheme == CrossPointSettings::UI_THEME::RADAR) {
    char radioBuf[48];
    char sysBuf[32];
    snprintf(radioBuf, sizeof(radioBuf), "wifi:%s  ble:OFF",
             wifiConnected ? "ON " : "OFF");
    snprintf(sysBuf, sizeof(sysBuf), "heap:%luK", (unsigned long)(freeHeap / 1024));
    RadarHomeStatus status { radioBuf, sysBuf, static_cast<int>(batteryPercent) };
    RadarHomeRenderer::draw(renderer, kRadarNodes, selectorIndex, status);
    const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), "<", ">");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // === STATUS BAR (top 44px) ===
  drawStatusBar();

  // === TILE GRID ===
  constexpr int statusBarH = 40;
  constexpr int buttonHintsH = 40;
  constexpr int sidePad = 14;
  constexpr int tileGap = 6;
  constexpr int gridTop = statusBarH + 8;
  const int gridBottom = pageHeight - buttonHintsH - 2;
  const int gridHeight = gridBottom - gridTop;

  const int tileW = (pageWidth - sidePad * 2 - tileGap) / COLS;
  const int tileH = (gridHeight - tileGap * (ROWS - 1)) / ROWS;

  for (int i = 0; i < ITEM_COUNT; i++) {
    int row = i / COLS;
    int col = i % COLS;
    int x = sidePad + col * (tileW + tileGap);
    int y = gridTop + row * (tileH + tileGap);
    drawTile(i, x, y, tileW, tileH, i == selectorIndex);
  }

  // === BUTTON HINTS ===
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "<", ">");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, "^", "v");

  renderer.displayBuffer();
}

void AppsMenuActivity::refreshSystemInfo() {
  freeHeap = esp_get_free_heap_size();
  uptimeSeconds = (unsigned long)(esp_timer_get_time() / 1000000LL);
  batteryPercent = (uint8_t)powerManager.getBatteryPercentage();
  wifiConnected = (WiFi.status() == WL_CONNECTED);
  wifiRssi = wifiConnected ? (int8_t)WiFi.RSSI() : 0;
  lastInfoRefresh = millis();

  unsigned long hrs = uptimeSeconds / 3600;
  unsigned long mins = (uptimeSeconds % 3600) / 60;
  if (hrs > 0) {
    snprintf(uptimeStr, sizeof(uptimeStr), "%luh%02lum", hrs, mins);
  } else {
    snprintf(uptimeStr, sizeof(uptimeStr), "%lum", mins);
  }

}

void AppsMenuActivity::loadLastUsed() {
  for (int i = 0; i < ITEM_COUNT; i++) {
    lastUsedName[i][0] = '\0';
    char path[40];
    snprintf(path, sizeof(path), "/biscuit/lastused_%d.txt", i);
    FsFile file;
    if (Storage.openFileForRead("APPS", path, file)) {
      int len = file.read((uint8_t*)lastUsedName[i], 31);
      if (len > 0) {
        lastUsedName[i][len] = '\0';
        // Strip trailing newline
        if (len > 0 && lastUsedName[i][len - 1] == '\n') {
          lastUsedName[i][len - 1] = '\0';
        }
      }
      file.close();
    }
  }
}

void AppsMenuActivity::drawStatusBar() const {
  const auto pageWidth = renderer.getScreenWidth();
  constexpr int pad = 14;

  // Left: branding
  renderer.drawText(UI_12_FONT_ID, pad, 10, "shortbread.", true, EpdFontFamily::BOLD);

  // Right side: build right-to-left to avoid overlap
  constexpr int sep = 8;  // extra gap between items
  const int pipeW = renderer.getTextWidth(SMALL_FONT_ID, " - ") + sep;
  int rightX = pageWidth - pad;

  // Uptime (rightmost)
  int uptimeW = renderer.getTextWidth(SMALL_FONT_ID, uptimeStr);
  renderer.drawText(SMALL_FONT_ID, rightX - uptimeW, 14, uptimeStr);
  rightX -= uptimeW + sep;

  // Separator
  renderer.drawText(SMALL_FONT_ID, rightX - pipeW + sep, 14, " - ");
  rightX -= pipeW;

  // Heap as % free
  char heapStr[8];
  uint32_t totalHeap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
  uint8_t heapPct = totalHeap ? (uint8_t)(freeHeap * 100 / totalHeap) : 0;
  snprintf(heapStr, sizeof(heapStr), "%u%%", heapPct);
  int heapW = renderer.getTextWidth(SMALL_FONT_ID, heapStr);
  renderer.drawText(SMALL_FONT_ID, rightX - heapW, 14, heapStr);
  rightX -= heapW + sep;

  // Separator
  renderer.drawText(SMALL_FONT_ID, rightX - pipeW + sep, 14, " - ");
  rightX -= pipeW;

  // WiFi: show "WiFi" text when connected, omit entirely when not
  if (wifiConnected) {
    constexpr const char* wifiLabel = "WiFi";
    int wifiLabelW = renderer.getTextWidth(SMALL_FONT_ID, wifiLabel);
    renderer.drawText(SMALL_FONT_ID, rightX - wifiLabelW, 14, wifiLabel);
    rightX -= wifiLabelW + sep;
    renderer.drawText(SMALL_FONT_ID, rightX - pipeW + sep, 14, " - ");
    rightX -= pipeW;
  }

  // Battery — drawBatteryRight draws percentage text at rect.y, icon at rect.y+6
  GUI.drawBatteryRight(renderer, Rect{rightX - 16, 14, 15, 12});

  // Separator line
  renderer.drawLine(pad, 38, pageWidth - pad, 38, true);
}

void AppsMenuActivity::drawTile(int index, int x, int y, int w, int h, bool selected) const {
  if (selected) {
    renderer.fillRect(x, y, w, h, true);
  } else {
    renderer.drawRect(x, y, w, h, true);
  }

  constexpr int pad = 10;

  // --- Zone 1: Top — category name + subtitle ---
  int nameY = y + pad;
  const char* name = "";
  const char* subtitle = "";
  int appCount = 0;

  switch (index) {
    case 0: name = "APPS";     subtitle = "Network & Utilities"; appCount = 8;  break;
    case 1: name = "GAMES";    subtitle = "Entertainment";      appCount = 11; break;
    case 2: name = "READER";   subtitle = "Books & OPDS";       appCount = 4;  break;
    case 3: name = "SETTINGS"; subtitle = "Display · Reader · Controls"; appCount = 0; break;
  }

  renderer.drawText(UI_12_FONT_ID, x + pad, nameY, name, !selected, EpdFontFamily::BOLD);
  nameY += renderer.getLineHeight(UI_12_FONT_ID) + 2;
  renderer.drawText(SMALL_FONT_ID, x + pad, nameY, subtitle, !selected);

  // --- Zone 2: Bottom-right — app count (skip for modules with 0) ---
  int countY = y + h - pad - renderer.getLineHeight(SMALL_FONT_ID);
  if (appCount > 0) {
    char countStr[16];
    snprintf(countStr, sizeof(countStr), "%d apps", appCount);
    int countW = renderer.getTextWidth(SMALL_FONT_ID, countStr);
    renderer.drawText(SMALL_FONT_ID, x + w - pad - countW, countY, countStr, !selected);
  }

  // --- Badge indicator (top-right corner of tile) ---
  int badge = 0;
  bool showBang = false;
  switch (index) {
    default: break;
  }

  if (badge > 0 || showBang) {
    int badgeX = x + w - 24;
    int badgeY = y + 6;
    // Draw badge background (inverted relative to tile)
    renderer.fillRect(badgeX, badgeY, 16, 16, !selected);
    // Draw badge text
    char badgeStr[4];
    if (showBang) {
      snprintf(badgeStr, sizeof(badgeStr), "!");
    } else {
      snprintf(badgeStr, sizeof(badgeStr), "%d", badge);
    }
    int bw = renderer.getTextWidth(SMALL_FONT_ID, badgeStr);
    renderer.drawText(SMALL_FONT_ID, badgeX + 8 - bw / 2, badgeY + 1, badgeStr, selected);
  }

  // --- Zone 3: Bottom-left — live status (selected tile only) ---
  if (selected) {
    char statusStr[48] = "";
    switch (index) {
      case 0:  // NETWORK
        snprintf(statusStr, sizeof(statusStr), wifiConnected ? "WiFi: on" : "WiFi: off");
        break;
      case 1:  // TOOLS
        snprintf(statusStr, sizeof(statusStr), "Heap: %luK", (unsigned long)(freeHeap / 1024));
        break;
      case 2:  // GAMES
      case 3:  // READER
        if (lastUsedName[index][0] != '\0') {
          snprintf(statusStr, sizeof(statusStr), "Last: %s", lastUsedName[index]);
        }
        break;
      default:
        break;
    }
    if (statusStr[0] != '\0') {
      int statusY = countY - renderer.getLineHeight(SMALL_FONT_ID) - 4;
      renderer.drawText(SMALL_FONT_ID, x + pad, statusY, statusStr, !selected);
    }
  }
}

