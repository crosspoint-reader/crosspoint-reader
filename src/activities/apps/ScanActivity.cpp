#include "ScanActivity.h"

#include <BLEAdvertisedDevice.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/FrameParser.h"
#include "util/RadioManager.h"

// Static member definition
PacketRingBuffer ScanActivity::ringBuf;

// ---------------------------------------------------------------------------
// Promiscuous callback — runs from WiFi task, must be IRAM and very fast
// ---------------------------------------------------------------------------

void IRAM_ATTR ScanActivity::promiscCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
    if (!buf) return;
    ringBuf.push(static_cast<const wifi_promiscuous_pkt_t*>(buf));
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void ScanActivity::onEnter() {
    Activity::onEnter();
    TARGETS.loadCache();   // Load cached targets from SD so idle screen shows real counts
    state = IDLE;
    phase = PAUSED;
    scanStartMs = 0;
    phaseStartMs = 0;
    lastDisplayMs = 0;
    currentChannel = 1;
    lastChannelHop = 0;
    totalAps = 0;
    totalClients = 0;
    totalBle = 0;
    packetsProcessed = 0;
    lastEvent[0] = '\0';
    browseIndex = 0;
    browseCount = 0;
    detailIndex = 0;
    categoryIndex = 0;
    browseType = TargetType::AP;
    ringBuf.clear();
    requestUpdate();
}

void ScanActivity::onExit() {
    Activity::onExit();
    stopScanning();  // handles WiFi promisc disable, BLE scan stop, and RADIO.shutdown()
}

// ---------------------------------------------------------------------------
// Phase management
// ---------------------------------------------------------------------------

void ScanActivity::startWifiPhase() {
    RADIO.ensureWifi();
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(promiscCallback);
    esp_wifi_set_promiscuous(true);
    currentChannel = 1;
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    phase = WIFI_PROMISC;
    phaseStartMs = millis();
    lastChannelHop = millis();
}

void ScanActivity::startBlePhase() {
    esp_wifi_set_promiscuous(false);
    RADIO.shutdown();
    RADIO.ensureBle();

    BLEScan* scan = BLEDevice::getScan();
    scan->setActiveScan(false);
    scan->setInterval(100);
    scan->setWindow(100);
    scan->clearResults();
    scan->start(static_cast<uint32_t>(BLE_PHASE_MS / 1000), false);

    phase = BLE_SCAN;
    phaseStartMs = millis();
}

void ScanActivity::stopScanning() {
    if (phase == WIFI_PROMISC) {
        esp_wifi_set_promiscuous(false);
    } else if (phase == BLE_SCAN) {
        BLEDevice::getScan()->stop();
    }
    phase = PAUSED;
    RADIO.shutdown();
    TARGETS.flush();
}

// ---------------------------------------------------------------------------
// Ring buffer drain and frame parsing
// ---------------------------------------------------------------------------

void ScanActivity::processRingBuffer() {
    CapturedPacket pkt;
    while (ringBuf.pop(pkt)) {
        FrameParser::processPacket(pkt);
        packetsProcessed++;
    }
}

// ---------------------------------------------------------------------------
// BLE phase completion — harvest results, transition back to WiFi
// ---------------------------------------------------------------------------

static void harvestBleResults() {
    BLEScan* scan = BLEDevice::getScan();
    BLEScanResults* results = scan->getResults();
    if (!results) return;
    const int count = results->getCount();
    for (int i = 0; i < count; i++) {
        BLEAdvertisedDevice d = results->getDevice(i);
        Target t;
        t.type = TargetType::BLE;
        const uint8_t* addr = d.getAddress().getNative();
        memcpy(t.mac, addr, 6);
        if (d.haveName()) {
            strncpy(t.name, d.getName().c_str(), sizeof(t.name) - 1);
            t.name[sizeof(t.name) - 1] = '\0';
        }
        t.rssi = d.getRSSI();
        t.lastSeen = static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL);
        t.firstSeen = t.lastSeen;
        TARGETS.addOrUpdate(t);
    }
    scan->clearResults();
}

// ---------------------------------------------------------------------------
// Stats refresh
// ---------------------------------------------------------------------------

void ScanActivity::updateStats() {
    totalAps     = TARGETS.countByType(TargetType::AP);
    totalClients = TARGETS.countByType(TargetType::STA);
    totalBle     = TARGETS.countByType(TargetType::BLE);
}

// ---------------------------------------------------------------------------
// Browse list refresh
// ---------------------------------------------------------------------------

void ScanActivity::refreshBrowseList() {
    browseCount = 0;
    TARGETS.getSorted(browseType, browseList, 50, browseCount, 0);
    browseIndex = 0;
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void ScanActivity::loop() {
    const unsigned long now = millis();

    if (state == IDLE) {
        if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
            state = SCANNING;
            scanStartMs = now;
            packetsProcessed = 0;
            ringBuf.clear();
            TARGETS.clear();
            startWifiPhase();
            requestUpdate();
            return;
        }
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
            finish();
            return;
        }
        return;
    }

    if (state == SCANNING) {
        // Drain ring buffer every loop iteration
        processRingBuffer();

        if (phase == WIFI_PROMISC) {
            // Channel hopping
            if (now - lastChannelHop >= CHANNEL_HOP_MS) {
                lastChannelHop = now;
                currentChannel = static_cast<uint8_t>((currentChannel % 13) + 1);
                esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
            }

            // Phase transition: WiFi -> BLE
            if (now - phaseStartMs >= WIFI_PHASE_MS) {
                startBlePhase();
            }
        } else if (phase == BLE_SCAN) {
            // Phase transition: BLE -> WiFi (either time-based or scan finished)
            BLEScan* scan = BLEDevice::getScan();
            const bool bleDone = (now - phaseStartMs >= BLE_PHASE_MS) || !scan->isScanning();
            if (bleDone) {
                harvestBleResults();
                RADIO.shutdown();
                startWifiPhase();
            }
        }

        // Update stats periodically
        updateStats();

        // Display throttle
        if (now - lastDisplayMs >= DISPLAY_INTERVAL_MS) {
            lastDisplayMs = now;
            requestUpdate();
        }

        // Button handling
        if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
            stopScanning();
            categoryIndex = 0;
            state = BROWSE_CATEGORIES;
            requestUpdate();
            return;
        }
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
            stopScanning();
            state = IDLE;
            requestUpdate();
            return;
        }
        return;
    }

    if (state == BROWSE_CATEGORIES) {
        static constexpr int CATEGORY_COUNT = 3;

        if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
            categoryIndex = ButtonNavigator::previousIndex(categoryIndex, CATEGORY_COUNT);
            requestUpdate();
            return;
        }
        if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
            categoryIndex = ButtonNavigator::nextIndex(categoryIndex, CATEGORY_COUNT);
            requestUpdate();
            return;
        }
        if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
            browseType = (categoryIndex == 0) ? TargetType::AP
                       : (categoryIndex == 1) ? TargetType::STA
                                              : TargetType::BLE;
            refreshBrowseList();
            state = BROWSE_LIST;
            requestUpdate();
            return;
        }
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
            state = IDLE;
            requestUpdate();
            return;
        }
        return;
    }

    if (state == BROWSE_LIST) {
        // Up/Down navigation
        buttonNavigator.onNext([this] {
            browseIndex = ButtonNavigator::nextIndex(browseIndex, browseCount);
            requestUpdate();
        });
        buttonNavigator.onPrevious([this] {
            browseIndex = ButtonNavigator::previousIndex(browseIndex, browseCount);
            requestUpdate();
        });

        // Confirm: go to detail
        if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
            if (browseCount > 0) {
                detailIndex = browseIndex;
                state = TARGET_DETAIL;
                requestUpdate();
            }
            return;
        }

        // Back: return to category selection
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
            state = BROWSE_CATEGORIES;
            requestUpdate();
            return;
        }
        return;
    }

    if (state == TARGET_DETAIL) {
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
            state = BROWSE_LIST;
            requestUpdate();
            return;
        }
        // Confirm: would chain to HuntActivity — for now return to browse
        if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
            state = BROWSE_LIST;
            requestUpdate();
            return;
        }
        // PageForward: export profile to SD
        if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
            if (detailIndex >= 0 && detailIndex < browseCount && browseList[detailIndex]) {
                char path[64];
                snprintf(path, sizeof(path), "/biscuit/profiles/%02x%02x%02x%02x%02x%02x.json",
                         browseList[detailIndex]->mac[0], browseList[detailIndex]->mac[1],
                         browseList[detailIndex]->mac[2], browseList[detailIndex]->mac[3],
                         browseList[detailIndex]->mac[4], browseList[detailIndex]->mac[5]);
                Storage.mkdir("/biscuit");
                Storage.mkdir("/biscuit/profiles");
                TARGETS.exportProfile(browseList[detailIndex]->mac, path);
            }
            requestUpdate();
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Render dispatch
// ---------------------------------------------------------------------------

void ScanActivity::render(RenderLock&& lock) {
    switch (state) {
        case IDLE:              renderIdle();          break;
        case SCANNING:          renderDashboard();     break;
        case BROWSE_CATEGORIES: renderCategories();    break;
        case BROWSE_LIST:       renderBrowse();        break;
        case TARGET_DETAIL:     renderTargetDetail();  break;
        default:                renderIdle();          break;
    }
}

// ---------------------------------------------------------------------------
// renderIdle
// ---------------------------------------------------------------------------

void ScanActivity::renderIdle() const {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth  = renderer.getScreenWidth();

    renderer.clearScreen();

    GUI.drawHeader(renderer,
                   Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   "SCAN");

    const int leftPad = metrics.contentSidePadding;
    int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 10;
    const int fontH = renderer.getTextHeight(UI_10_FONT_ID);

    renderer.drawCenteredText(UI_12_FONT_ID, y, "Passive Wireless Intelligence", true, EpdFontFamily::BOLD);
    y += renderer.getTextHeight(UI_12_FONT_ID) + 16;

    renderer.drawText(UI_10_FONT_ID, leftPad, y, "Press OK to start passive scan.");
    y += fontH + 6;
    renderer.drawText(UI_10_FONT_ID, leftPad, y, "Alternates WiFi promiscuous + BLE scan.");
    y += fontH + 24;

    // Show current DB stats
    char buf[48];
    snprintf(buf, sizeof(buf), "Cached APs:      %d", TARGETS.countByType(TargetType::AP));
    renderer.drawText(UI_10_FONT_ID, leftPad, y, buf);
    y += fontH + 6;

    snprintf(buf, sizeof(buf), "Cached Clients:  %d", TARGETS.countByType(TargetType::STA));
    renderer.drawText(UI_10_FONT_ID, leftPad, y, buf);
    y += fontH + 6;

    snprintf(buf, sizeof(buf), "Cached BLE:      %d", TARGETS.countByType(TargetType::BLE));
    renderer.drawText(UI_10_FONT_ID, leftPad, y, buf);

    const auto labels = mappedInput.mapLabels("Back", "Start", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    renderer.displayBuffer();
}

// ---------------------------------------------------------------------------
// renderDashboard
// ---------------------------------------------------------------------------

void ScanActivity::renderDashboard() const {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth  = renderer.getScreenWidth();

    renderer.clearScreen();

    const char* phaseStr = (phase == WIFI_PROMISC) ? "WiFi" : "BLE";
    char headerSub[32];
    snprintf(headerSub, sizeof(headerSub), "Ch:%d  %s", currentChannel, phaseStr);

    GUI.drawHeader(renderer,
                   Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   "SCAN — ACTIVE",
                   headerSub);

    const int leftPad = metrics.contentSidePadding;
    int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 10;
    const int fontH = renderer.getTextHeight(UI_10_FONT_ID);
    const int lineGap = fontH + 8;

    char buf[64];

    snprintf(buf, sizeof(buf), "WiFi APs:     %d", totalAps);
    renderer.drawText(UI_10_FONT_ID, leftPad, y, buf);
    y += lineGap;

    snprintf(buf, sizeof(buf), "WiFi Clients: %d", totalClients);
    renderer.drawText(UI_10_FONT_ID, leftPad, y, buf);
    y += lineGap;

    snprintf(buf, sizeof(buf), "BLE Devices:  %d", totalBle);
    renderer.drawText(UI_10_FONT_ID, leftPad, y, buf);
    y += lineGap;

    snprintf(buf, sizeof(buf), "Packets:      %d", packetsProcessed);
    renderer.drawText(UI_10_FONT_ID, leftPad, y, buf);
    y += lineGap + 8;

    // Elapsed time
    const unsigned long elapsedSec = (millis() - scanStartMs) / 1000UL;
    const unsigned long elapsedMin = elapsedSec / 60;
    const unsigned long elapsedS   = elapsedSec % 60;
    snprintf(buf, sizeof(buf), "Elapsed: %lum %02lus", elapsedMin, elapsedS);
    renderer.drawText(UI_10_FONT_ID, leftPad, y, buf);
    y += lineGap + 8;

    // Last event
    if (lastEvent[0] != '\0') {
        snprintf(buf, sizeof(buf), "Last: %.44s", lastEvent);
        renderer.drawText(SMALL_FONT_ID, leftPad, y, buf);
    }

    const auto labels = mappedInput.mapLabels("Stop", "Browse", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    renderer.displayBuffer();
}

// ---------------------------------------------------------------------------
// renderCategories
// ---------------------------------------------------------------------------

void ScanActivity::renderCategories() const {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth  = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();

    renderer.clearScreen();

    GUI.drawHeader(renderer,
                   Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   "SCAN — BROWSE");

    const int listTop    = metrics.topPadding + metrics.headerHeight;
    const int hintsH     = metrics.buttonHintsHeight;
    const int listHeight = pageHeight - listTop - hintsH;

    const int apCount     = TARGETS.countByType(TargetType::AP);
    const int staCount    = TARGETS.countByType(TargetType::STA);
    const int bleCount    = TARGETS.countByType(TargetType::BLE);

    GUI.drawList(
        renderer,
        Rect{0, listTop, pageWidth, listHeight},
        3,
        categoryIndex,
        [apCount, staCount, bleCount](int i) -> std::string {
            char buf[32];
            if (i == 0)      snprintf(buf, sizeof(buf), "WiFi APs (%d)", apCount);
            else if (i == 1) snprintf(buf, sizeof(buf), "WiFi Clients (%d)", staCount);
            else             snprintf(buf, sizeof(buf), "BLE Devices (%d)", bleCount);
            return std::string(buf);
        },
        [](int) -> std::string { return ""; }
    );

    const auto labels = mappedInput.mapLabels("Back", "Open", "^", "v");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    GUI.drawSideButtonHints(renderer, "^", "v");

    renderer.displayBuffer();
}

// ---------------------------------------------------------------------------
// renderBrowse
// ---------------------------------------------------------------------------

void ScanActivity::renderBrowse() const {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth  = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();

    renderer.clearScreen();

    const char* catName = (browseType == TargetType::AP)  ? "WiFi APs"
                        : (browseType == TargetType::STA) ? "Clients"
                                                          : "BLE";
    char headerTitle[32];
    snprintf(headerTitle, sizeof(headerTitle), "SCAN — %s", catName);
    GUI.drawHeader(renderer,
                   Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   headerTitle);

    const int listTop    = metrics.topPadding + metrics.headerHeight;
    const int hintsH     = metrics.buttonHintsHeight;
    const int listHeight = pageHeight - listTop - hintsH;

    if (browseCount == 0) {
        renderer.drawCenteredText(UI_10_FONT_ID,
                                  listTop + listHeight / 2 - renderer.getTextHeight(UI_10_FONT_ID) / 2,
                                  "No targets found.");
    } else {
        GUI.drawList(
            renderer,
            Rect{0, listTop, pageWidth, listHeight},
            browseCount,
            browseIndex,
            [this](int i) -> std::string {
                if (i < 0 || i >= browseCount || !browseList[i]) return "";
                const Target* t = browseList[i];
                char mac[20];
                snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                         t->mac[0], t->mac[1], t->mac[2], t->mac[3], t->mac[4], t->mac[5]);
                return std::string(mac);
            },
            [this](int i) -> std::string {
                if (i < 0 || i >= browseCount || !browseList[i]) return "";
                const Target* t = browseList[i];
                char sub[48];
                const char* typeStr = (t->type == TargetType::AP)  ? "AP"
                                    : (t->type == TargetType::STA) ? "Client"
                                                                    : "BLE";
                if (t->type == TargetType::AP && t->ssid[0] != '\0') {
                    snprintf(sub, sizeof(sub), "[%s] %s  RSSI:%d", typeStr, t->ssid, (int)t->rssi);
                } else if (t->type == TargetType::BLE && t->name[0] != '\0') {
                    snprintf(sub, sizeof(sub), "[%s] %s  RSSI:%d", typeStr, t->name, (int)t->rssi);
                } else {
                    snprintf(sub, sizeof(sub), "[%s]  RSSI:%d  Ch:%d", typeStr, (int)t->rssi, (int)t->channel);
                }
                return std::string(sub);
            }
        );
    }

    const auto labels = mappedInput.mapLabels("Back", "Detail", "^", "v");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    GUI.drawSideButtonHints(renderer, "^", "v");

    renderer.displayBuffer();
}

// ---------------------------------------------------------------------------
// renderTargetDetail
// ---------------------------------------------------------------------------

void ScanActivity::renderTargetDetail() const {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth  = renderer.getScreenWidth();

    renderer.clearScreen();

    if (detailIndex < 0 || detailIndex >= browseCount || !browseList[detailIndex]) {
        GUI.drawHeader(renderer,
                       Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                       "SCAN — DETAIL");
        renderer.displayBuffer();
        return;
    }

    const Target* t = browseList[detailIndex];

    char mac[20];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             t->mac[0], t->mac[1], t->mac[2], t->mac[3], t->mac[4], t->mac[5]);

    GUI.drawHeader(renderer,
                   Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   "DETAIL",
                   mac);

    const int leftPad = metrics.contentSidePadding;
    int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 6;
    const int fontH   = renderer.getTextHeight(UI_10_FONT_ID);
    const int smallH  = renderer.getTextHeight(SMALL_FONT_ID);
    const int lineGap = fontH + 6;

    char buf[64];

    // Type
    const char* typeStr = (t->type == TargetType::AP)  ? "WiFi AP"
                        : (t->type == TargetType::STA) ? "WiFi Client"
                                                       : "BLE Device";
    snprintf(buf, sizeof(buf), "Type:    %s", typeStr);
    renderer.drawText(UI_10_FONT_ID, leftPad, y, buf);
    y += lineGap;

    // SSID / Name
    if (t->type == TargetType::AP && t->ssid[0] != '\0') {
        snprintf(buf, sizeof(buf), "SSID:    %.48s", t->ssid);
        renderer.drawText(UI_10_FONT_ID, leftPad, y, buf);
        y += lineGap;
    } else if (t->type == TargetType::BLE && t->name[0] != '\0') {
        snprintf(buf, sizeof(buf), "Name:    %.48s", t->name);
        renderer.drawText(UI_10_FONT_ID, leftPad, y, buf);
        y += lineGap;
    }

    // Vendor
    if (t->vendor[0] != '\0') {
        snprintf(buf, sizeof(buf), "Vendor:  %.48s", t->vendor);
        renderer.drawText(UI_10_FONT_ID, leftPad, y, buf);
        y += lineGap;
    }

    // OS
    if (t->os[0] != '\0') {
        snprintf(buf, sizeof(buf), "OS:      %.48s", t->os);
        renderer.drawText(UI_10_FONT_ID, leftPad, y, buf);
        y += lineGap;
    }

    // RSSI + channel
    snprintf(buf, sizeof(buf), "RSSI:    %d dBm   Ch: %d", (int)t->rssi, (int)t->channel);
    renderer.drawText(UI_10_FONT_ID, leftPad, y, buf);
    y += lineGap;

    // Auth + PMF (WiFi AP only)
    if (t->type == TargetType::AP) {
        static const char* const AUTH_NAMES[] = {"Open", "WEP", "WPA", "WPA2", "WPA3"};
        const char* authName = (t->authType < 5) ? AUTH_NAMES[t->authType] : "?";
        snprintf(buf, sizeof(buf), "Auth:    %s   PMF: %s   WPS: %s",
                 authName,
                 t->pmf  ? "Yes" : "No",
                 t->wps  ? "Yes" : "No");
        renderer.drawText(UI_10_FONT_ID, leftPad, y, buf);
        y += lineGap;

        snprintf(buf, sizeof(buf), "Clients: %d   Handshake: %s   PMKID: %s",
                 (int)t->clientCount,
                 t->hasHandshake ? "Yes" : "No",
                 t->hasPmkid     ? "Yes" : "No");
        renderer.drawText(UI_10_FONT_ID, leftPad, y, buf);
        y += lineGap;
    }

    // First / last seen
    snprintf(buf, sizeof(buf), "First:   %lus ago", (unsigned long)(esp_timer_get_time() / 1000000ULL) - (unsigned long)t->firstSeen);
    renderer.drawText(SMALL_FONT_ID, leftPad, y, buf);
    y += smallH + 4;

    snprintf(buf, sizeof(buf), "Last:    %lus ago", (unsigned long)(esp_timer_get_time() / 1000000ULL) - (unsigned long)t->lastSeen);
    renderer.drawText(SMALL_FONT_ID, leftPad, y, buf);
    y += smallH + 4;

    // Probes (WiFi Client)
    if (t->type == TargetType::STA && t->probeCount > 0) {
        renderer.drawText(SMALL_FONT_ID, leftPad, y, "Probes:");
        y += smallH + 2;
        for (int i = 0; i < t->probeCount && i < 5; i++) {
            snprintf(buf, sizeof(buf), "  %.58s", t->probes[i]);
            renderer.drawText(SMALL_FONT_ID, leftPad, y, buf);
            y += smallH + 2;
        }
    }

    const auto labels = mappedInput.mapLabels("Back", "Hunt", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    GUI.drawSideButtonHints(renderer, "", "Export");

    renderer.displayBuffer();
}
