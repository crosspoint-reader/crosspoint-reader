#include "AppsMenuActivity.h"

#include <I18n.h>

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <Xtc.h>
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
#include "activities/home/FileBrowserActivity.h"
#include "activities/home/RecentBooksActivity.h"
#include "activities/browser/OpdsBookBrowserActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/network/CrossPointWebServerActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <WiFi.h>
#include <HalPowerManager.h>
#include <HalStorage.h>


void AppsMenuActivity::loadRecentBooks() {
  recentBooks.clear();
  memset(progressStr, 0, sizeof(progressStr));
  for (const auto& b : RECENT_BOOKS.getBooks()) {
    if ((int)recentBooks.size() >= MAX_RECENT) break;
    if (!Storage.exists(b.path.c_str())) continue;

    int idx = (int)recentBooks.size();
    recentBooks.push_back(b);

    // Read progress.bin cheaply without loading the epub
    std::string cachePath;
    if (FsHelpers::hasEpubExtension(b.path)) {
      cachePath = Epub(b.path, "/.crosspoint").getCachePath();
    } else if (FsHelpers::hasXtcExtension(b.path)) {
      cachePath = Xtc(b.path, "/.crosspoint").getCachePath();
    }
    if (cachePath.empty()) continue;

    std::string progressPath = cachePath + "/progress.bin";
    FsFile f;
    if (Storage.openFileForRead("APM", progressPath, f)) {
      uint8_t data[6] = {};
      int n = f.read(data, 6);
      f.close();
      if (n >= 4) {
        int spineIndex = data[0] + (data[1] << 8);
        int curPage    = data[2] + (data[3] << 8);
        int pageCount  = (n >= 6) ? data[4] + (data[5] << 8) : 0;
        if (pageCount > 0) {
          snprintf(progressStr[idx], sizeof(progressStr[idx]),
                   "Ch.%d p.%d/%d", spineIndex + 1, curPage + 1, pageCount);
        } else {
          snprintf(progressStr[idx], sizeof(progressStr[idx]),
                   "Ch.%d p.%d", spineIndex + 1, curPage + 1);
        }
      }
    }
  }
}

void AppsMenuActivity::loadCovers() {
  for (RecentBook& book : recentBooks) {
    if (book.coverBmpPath.empty()) continue;
    std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverThumbH);
    if (Storage.exists(thumbPath.c_str())) continue;

    if (FsHelpers::hasEpubExtension(book.path)) {
      Epub epub(book.path, "/.crosspoint");
      epub.load(false, true);
      if (!epub.generateThumbBmp(coverThumbH)) book.coverBmpPath = "";
    } else if (FsHelpers::hasXtcExtension(book.path)) {
      Xtc xtc(book.path, "/.crosspoint");
      if (xtc.load()) {
        if (!xtc.generateThumbBmp(coverThumbH)) book.coverBmpPath = "";
      }
    }
  }
  coversLoaded = true;
  coversLoading = false;
  requestUpdate();
}

void AppsMenuActivity::onEnter() {
  Activity::onEnter();
  selectorIndex = 0;
  coversLoaded = false;
  coversLoading = false;
  firstRenderDone = false;
  refreshSystemInfo();
  loadLastUsed();
  loadRecentBooks();
  requestUpdate();
}

void AppsMenuActivity::loop() {
  const int recentCount = (int)recentBooks.size();

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (!isRecentSelected()) {
      int col = getTileCol() + 1;
      int row = getTileRow();
      if (col >= COLS) { col = 0; row = (row + 1) % TILE_ROWS; }
      selectorIndex = row * COLS + col;
      requestUpdate();
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (!isRecentSelected()) {
      int col = getTileCol() - 1;
      int row = getTileRow();
      if (col < 0) { col = COLS - 1; row = (row - 1 + TILE_ROWS) % TILE_ROWS; }
      selectorIndex = row * COLS + col;
      requestUpdate();
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (isRecentSelected()) {
      int next = recentIdx() + 1;
      selectorIndex = (next >= recentCount) ? 0 : ITEM_COUNT + next;
    } else {
      int row = getTileRow() + 1;
      if (row >= TILE_ROWS)
        selectorIndex = recentCount > 0 ? ITEM_COUNT : 0;
      else
        selectorIndex = row * COLS + getTileCol();
    }
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (isRecentSelected()) {
      int prev = recentIdx() - 1;
      selectorIndex = (prev < 0) ? ITEM_COUNT - 1 : ITEM_COUNT + prev;
    } else {
      int row = getTileRow() - 1;
      if (row < 0)
        selectorIndex = recentCount > 0 ? ITEM_COUNT + recentCount - 1 : ITEM_COUNT - 1;
      else
        selectorIndex = row * COLS + getTileCol();
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

  // === CONFIRM: open recent book or category ===
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (isRecentSelected() && recentIdx() < (int)recentBooks.size()) {
      activityManager.goToReader(recentBooks[recentIdx()].path);
      return;
    }
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
              {"WiFi Transfer", "Upload/download via WiFi", UIIcon::Transfer, [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<CrossPointWebServerActivity>(r, m); }},
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

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  drawStatusBar();

  constexpr int statusBarH = 40;
  constexpr int buttonHintsH = 40;
  constexpr int sidePad = 14;
  constexpr int tileGap = 6;
  constexpr int gridTop = statusBarH + 8;
  const int pageBottom = pageHeight - buttonHintsH - 4;
  const int totalH = pageBottom - gridTop;

  // Top 44% for tiles (2×2)
  const int tileSectionH = totalH * 44 / 100;
  const int tileW = (pageWidth - sidePad * 2 - tileGap) / COLS;
  const int tileH = (tileSectionH - tileGap) / TILE_ROWS;

  for (int i = 0; i < ITEM_COUNT; i++) {
    int row = i / COLS;
    int col = i % COLS;
    int x = sidePad + col * (tileW + tileGap);
    int y = gridTop + row * (tileH + tileGap);
    drawTile(i, x, y, tileW, tileH, i == selectorIndex);
  }

  // Divider
  const int divY = gridTop + tileSectionH + 6;
  renderer.drawLine(sidePad, divY, pageWidth - sidePad, divY, true);

  // Bottom section — recent books
  const int recentsTop = divY + 8;
  const int recentsH = pageBottom - recentsTop;
  const int headerH = renderer.getLineHeight(SMALL_FONT_ID) + 4;
  const int booksH = recentsH - headerH;
  const int bookH = (recentBooks.empty()) ? booksH : booksH / (int)recentBooks.size();
  // Generate tall enough that cover width > tile width (drawBitmap only downscales).
  // tileW = pageWidth - sidePad*2. For a 2:3 portrait cover: coverW = thumbH * 2/3.
  // Need coverW > tileW → thumbH > tileW * 3/2.
  coverThumbH = ((pageWidth - sidePad * 2) * 3) / 2 + 50;
  drawRecentBooks(sidePad, recentsTop, pageWidth - sidePad * 2, recentsH);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "<", ">");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, "^", "v");

  renderer.displayBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!coversLoaded && !coversLoading && !recentBooks.empty()) {
    coversLoading = true;
    loadCovers();
  }
}

void AppsMenuActivity::drawRecentBooks(int x, int y, int w, int h) const {
  // Section header
  renderer.drawText(SMALL_FONT_ID, x, y, "RECENT", true, EpdFontFamily::BOLD);
  const int headerH = renderer.getLineHeight(SMALL_FONT_ID) + 4;

  if (recentBooks.empty()) {
    renderer.drawText(SMALL_FONT_ID, x, y + headerH + 4, "No recent books");
    return;
  }

  const int booksTop = y + headerH;
  const int booksH = h - headerH;
  const int titleLineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int authorLineH = renderer.getLineHeight(SMALL_FONT_ID);
  constexpr int pad = 8;

  // Which book to display — selected recent when navigating recents, else book 0
  const int showIdx = (isRecentSelected() && recentIdx() < (int)recentBooks.size())
                      ? recentIdx() : 0;
  const RecentBook& shown = recentBooks[showIdx];

  // Full-section border (selected = inverted, otherwise plain rect)
  const bool recSel = isRecentSelected();
  if (recSel) renderer.fillRect(x, booksTop, w, booksH, true);
  else        renderer.drawRect(x, booksTop, w, booksH, true);

  // Cover fills the entire section
  const uint8_t opacity = SETTINGS.coverOpacity;
  if (coversLoaded && opacity != CrossPointSettings::COVER_OFF &&
      !shown.coverBmpPath.empty() && coverThumbH > 0 && !recSel) {
    std::string thumbPath = UITheme::getCoverThumbPath(shown.coverBmpPath, coverThumbH);
    FsFile coverFile;
    if (Storage.openFileForRead("APM", thumbPath, coverFile)) {
      Bitmap bitmap(coverFile);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        int bmpW = bitmap.getWidth();
        int bmpH = bitmap.getHeight();
        float cropY = 0.0f;
        if (bmpW > 0 && bmpH > 0) {
          float widthScale = (float)(w - 2) / (float)bmpW;
          float scaledH = bmpH * widthScale;
          if (scaledH > booksH - 2) cropY = 1.0f - ((float)(booksH - 2) / scaledH);
        }
        renderer.drawBitmap(bitmap, x + 1, booksTop + 1, w - 2, booksH - 2, 0.0f, cropY);
        if (opacity == CrossPointSettings::COVER_LIGHT) {
          for (int fy = booksTop + 1; fy < booksTop + booksH - 1; fy++)
            for (int fx = x + 1; fx < x + w - 1; fx++)
              if (!(fx % 2 == 0 && fy % 2 == 0))
                renderer.drawPixel(fx, fy, false);
        } else if (opacity == CrossPointSettings::COVER_MEDIUM) {
          for (int fy = booksTop + 1; fy < booksTop + booksH - 1; fy++)
            for (int fx = x + 1; fx < x + w - 1; fx++)
              if ((fx + fy) % 2 != 0)
                renderer.drawPixel(fx, fy, false);
        }
      }
      coverFile.close();
    }
  }

  // Book info — pinned to bottom of section
  const int infoH = titleLineH + authorLineH + 6;
  const int infoY = booksTop + booksH - infoH - pad;
  const std::string& title = shown.title.empty() ? shown.path : shown.title;
  std::string displayTitle = title;
  const int maxW = w - pad * 2;
  while (displayTitle.size() > 4 && renderer.getTextWidth(UI_10_FONT_ID, displayTitle.c_str()) > maxW)
    displayTitle.resize(displayTitle.size() - 4);
  if (displayTitle.size() < title.size()) displayTitle += "...";

  renderer.drawText(UI_10_FONT_ID, x + pad, infoY, displayTitle.c_str(), !recSel, EpdFontFamily::BOLD);

  if (!shown.author.empty())
    renderer.drawText(SMALL_FONT_ID, x + pad, infoY + titleLineH + 2, shown.author.c_str(), !recSel);

  if (progressStr[showIdx][0] != '\0') {
    int pW = renderer.getTextWidth(SMALL_FONT_ID, progressStr[showIdx]);
    renderer.drawText(SMALL_FONT_ID, x + w - pad - pW, infoY + titleLineH + 2,
                      progressStr[showIdx], !recSel);
  }

  // Book index indicator (e.g. "2 / 3") top-right
  if ((int)recentBooks.size() > 1) {
    char idxStr[8];
    snprintf(idxStr, sizeof(idxStr), "%d / %d", showIdx + 1, (int)recentBooks.size());
    int idxW = renderer.getTextWidth(SMALL_FONT_ID, idxStr);
    renderer.drawText(SMALL_FONT_ID, x + w - pad - idxW, booksTop + pad, idxStr, !recSel);
  }

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

