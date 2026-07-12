#include "OffenseMenuActivity.h"

#include <I18n.h>
#include <cstdio>

#include "AppCategoryActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

// --- Scan sub-tile activities ---
#include "WifiScannerActivity.h"
#include "BleScannerActivity.h"
#include "ScanActivity.h"
#include "HuntActivity.h"

// --- Profile sub-tile activities ---
#include "HostScannerActivity.h"
#include "SignalTriangulationActivity.h"

// --- Operations sub-tile activities ---
#include "WifiTestActivity.h"
#include "CaptivePortalActivity.h"
#include "BleSpamActivity.h"
#include "BleKeyboardActivity.h"
#include "AirTagTestActivity.h"
#include "ApClonerActivity.h"
#include "FireActivity.h"

// --- Capture sub-tile activities ---
#include "LootActivity.h"
#include "CredentialViewerActivity.h"
#include "ProbeSnifferActivity.h"
#include "QuickWipeActivity.h"

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void OffenseMenuActivity::onEnter() {
    Activity::onEnter();
    selectorIndex = 0;

    if (!RADIO.isDisclaimerAcknowledged()) {
        disclaimerShown = true;
    }

    requestUpdate();
}

// ---------------------------------------------------------------------------
// Loop — 2D grid navigation
// ---------------------------------------------------------------------------

void OffenseMenuActivity::loop() {
    // Disclaimer gate
    if (disclaimerShown) {
        if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
            RADIO.setDisclaimerAcknowledged();
            disclaimerShown = false;
            requestUpdate();
        }
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
            finish();
            return;
        }
        return;
    }

    // Left / Right — move between columns (with row wrap)
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
        int col = getCol();
        int row = getRow();
        col++;
        if (col >= COLS) { col = 0; row = (row + 1) % ROWS; }
        selectorIndex = row * COLS + col;
        if (selectorIndex >= ITEM_COUNT) selectorIndex = 0;
        requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
        int col = getCol();
        int row = getRow();
        col--;
        if (col < 0) { col = COLS - 1; row = (row - 1 + ROWS) % ROWS; }
        selectorIndex = row * COLS + col;
        if (selectorIndex >= ITEM_COUNT) selectorIndex = ITEM_COUNT - 1;
        requestUpdate();
    }

    // Up / Down — move between rows
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

    // Confirm — open sub-tile
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        openSubTile(selectorIndex);
    }

    // Back — return to dashboard
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        finish();
    }
}

// ---------------------------------------------------------------------------
// openSubTile — build AppCategoryActivity for each phase
// ---------------------------------------------------------------------------

void OffenseMenuActivity::openSubTile(int index) {
    std::unique_ptr<Activity> app;

    switch (index) {
        case 0: {
            // ── SCAN — target discovery ──
            std::vector<AppCategoryActivity::AppEntry> e = {
                AppCategoryActivity::SectionHeader("SCANNING"),
                {"WiFi Scan", "Discover APs + clients", UIIcon::Wifi,
                 [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<WifiScannerActivity>(r, m); }},
                {"BLE Scan", "Discover BLE devices", UIIcon::Hotspot,
                 [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<BleScannerActivity>(r, m); }},
                {"Full Sweep", "WiFi + BLE combined passive scan", UIIcon::Hotspot,
                 [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<ScanActivity>(r, m); }},
                AppCategoryActivity::SectionHeader("SAVED"),
                {"Saved Targets", "Browse cached target database", UIIcon::Recent,
                 [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<HuntActivity>(r, m); }},
            };
            app = std::make_unique<AppCategoryActivity>(renderer, mappedInput, "Scan", std::move(e), false, -1);
            break;
        }

        case 1: {
            // ── PROFILE — target analysis ──
            std::vector<AppCategoryActivity::AppEntry> e = {
                {"Target Profiler", "Select + analyze a target", UIIcon::File,
                 [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<HuntActivity>(r, m); }},
                {"Client Enum", "Devices connected to target AP", UIIcon::Wifi,
                 [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<HuntActivity>(r, m); }},
                {"Host Scanner", "Find devices on local network", UIIcon::Wifi,
                 [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<HostScannerActivity>(r, m); }},
                {"Vuln Assessment", "Check encryption + WPS settings", UIIcon::Settings,
                 [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<HuntActivity>(r, m); }},
                {"Signal Locator", "Estimate AP position from RSSI", UIIcon::Wifi,
                 [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<SignalTriangulationActivity>(r, m); }},
            };
            app = std::make_unique<AppCategoryActivity>(renderer, mappedInput, "Profile", std::move(e), false, -1);
            break;
        }

        case 2: {
            // ── OPERATIONS — broadcast + testing tools ──
            std::vector<AppCategoryActivity::AppEntry> e = {
                AppCategoryActivity::SectionHeader("WIRELESS"),
                {"Deauther", "Targeted disassociation testing", UIIcon::Wifi,
                 [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<WifiTestActivity>(r, m); }},
                {tr(STR_CAPTIVE_PORTAL), "Network portal for testing", UIIcon::Hotspot,
                 [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<CaptivePortalActivity>(r, m); }},
                {"Beacon Flood", "Broadcast 30 random SSIDs", UIIcon::Wifi,
                 [](GfxRenderer& r, MappedInputManager& m) -> std::unique_ptr<Activity> {
                     auto f = std::make_unique<FireActivity>(r, m);
                     f->setAttack(5);  // ATK_BEACON_FLOOD — universal, no target needed
                     return f;
                 }},
                {"SSID Clone", "Clone a WiFi AP (open, same channel)", UIIcon::Wifi,
                 [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<ApClonerActivity>(r, m); }},
                AppCategoryActivity::SectionHeader("BLE"),
                {"BLE Spam", "Proximity/Fast Pair/Swift Pair flood", UIIcon::Hotspot,
                 [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<BleSpamActivity>(r, m); }},
                {tr(STR_BLE_KEYBOARD), "HID keyboard emulation", UIIcon::Transfer,
                 [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<BleKeyboardActivity>(r, m); }},
                {"AirTag Spoof", "Device location testing", UIIcon::Hotspot,
                 [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<AirTagTestActivity>(r, m); }},
            };
            app = std::make_unique<AppCategoryActivity>(renderer, mappedInput, "Attack", std::move(e), true, -1);
            break;
        }

        case 3: {
            // ── CAPTURE — review + export captured data ──
            std::vector<AppCategoryActivity::AppEntry> e = {
                {"Captured Data", "Handshakes, creds, PCAPs, BLE", UIIcon::File,
                 [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<LootActivity>(r, m); }},
                {"Credential Viewer", "View portal captured credentials", UIIcon::Text,
                 [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<CredentialViewerActivity>(r, m); }},
                {"Probe Log", "Capture WiFi probe requests", UIIcon::Wifi,
                 [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<ProbeSnifferActivity>(r, m); }},
                {"Scan History", "Browse previously found targets", UIIcon::Recent,
                 [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<HuntActivity>(r, m); }},
                AppCategoryActivity::SectionHeader("MANAGE"),
                {"Wipe Captures", "Delete all captured data", UIIcon::Folder,
                 [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<QuickWipeActivity>(r, m); }},
            };
            app = std::make_unique<AppCategoryActivity>(renderer, mappedInput, "Capture", std::move(e), false, -1);
            break;
        }
    }

    if (app) activityManager.pushActivity(std::move(app));
}

// ---------------------------------------------------------------------------
// Render — header + 2×2 tile grid + button hints
// ---------------------------------------------------------------------------

void OffenseMenuActivity::render(RenderLock&&) {
    renderer.clearScreen();

    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth  = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();

    // --- Disclaimer overlay ---
    if (disclaimerShown) {
        GUI.drawHeader(renderer,
                       Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                       "OFFENSE");
        renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, tr(STR_DISCLAIMER));
        const auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_CONFIRM), "", "");
        GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
        renderer.displayBuffer();
        return;
    }

    // --- Header ---
    GUI.drawHeader(renderer,
                   Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   "OFFENSE");

    // --- 2×2 tile grid ---
    constexpr int sidePad = 14;
    constexpr int tileGap = 8;
    const int gridTop    = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int gridBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
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

    // --- Button hints ---
    const auto labels = mappedInput.mapLabels(
        tr(STR_BACK), tr(STR_SELECT),
        "<", ">");  // ◀ ▶
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    GUI.drawSideButtonHints(renderer,
        "^", "v");   // ▲ ▼

    renderer.displayBuffer();
}

// ---------------------------------------------------------------------------
// drawTile — individual tile within the 2×2 grid
// ---------------------------------------------------------------------------

void OffenseMenuActivity::drawTile(int index, int x, int y, int w, int h, bool selected) const {
    if (selected) {
        renderer.fillRect(x, y, w, h, true);
    } else {
        renderer.drawRect(x, y, w, h, true);
    }

    constexpr int pad = 10;
    int nameY = y + pad;

    const char* name     = "";
    const char* subtitle = "";
    int itemCount        = 0;

    switch (index) {
        case 0: name = "SCAN";    subtitle = "Target discovery";  itemCount = 4;  break;
        case 1: name = "PROFILE"; subtitle = "Target analysis";   itemCount = 5;  break;
        case 2: name = "ATTACK";  subtitle = "Tools & testing";   itemCount = 6;  break;
        case 3: name = "CAPTURE"; subtitle = "Review & export";   itemCount = 5;  break;
    }

    renderer.drawText(UI_12_FONT_ID, x + pad, nameY, name, !selected, EpdFontFamily::BOLD);
    nameY += renderer.getLineHeight(UI_12_FONT_ID) + 2;
    renderer.drawText(SMALL_FONT_ID, x + pad, nameY, subtitle, !selected);

    // Item count — bottom-right
    if (itemCount > 0) {
        char countStr[16];
        snprintf(countStr, sizeof(countStr), "%d items", itemCount);
        int countW = renderer.getTextWidth(SMALL_FONT_ID, countStr);
        int countY = y + h - pad - renderer.getLineHeight(SMALL_FONT_ID);
        renderer.drawText(SMALL_FONT_ID, x + w - pad - countW, countY, countStr, !selected);
    }
}
