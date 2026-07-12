#pragma once
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "util/PacketRingBuffer.h"
#include "util/TargetDB.h"

class ScanActivity final : public Activity {
public:
    explicit ScanActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
        : Activity("Scan", renderer, mappedInput) {}

    void onEnter() override;
    void onExit() override;
    void loop() override;
    void render(RenderLock&&) override;
    bool preventAutoSleep() override { return state == SCANNING; }
    bool skipLoopDelay() override { return state == SCANNING; }

private:
    enum State { IDLE, SCANNING, BROWSE_CATEGORIES, BROWSE_LIST, TARGET_DETAIL };
    enum ScanPhase { WIFI_PROMISC, BLE_SCAN, PAUSED };

    State state = IDLE;
    ScanPhase phase = PAUSED;

    // Scan timing
    unsigned long scanStartMs = 0;
    unsigned long phaseStartMs = 0;
    unsigned long lastDisplayMs = 0;
    static constexpr unsigned long WIFI_PHASE_MS = 4000;
    static constexpr unsigned long BLE_PHASE_MS = 3000;
    static constexpr unsigned long DISPLAY_INTERVAL_MS = 500;

    // WiFi channel hopping
    uint8_t currentChannel = 1;
    unsigned long lastChannelHop = 0;
    static constexpr unsigned long CHANNEL_HOP_MS = 500;

    // Stats
    int totalAps = 0;
    int totalClients = 0;
    int totalBle = 0;
    int packetsProcessed = 0;
    char lastEvent[48] = {};

    // Browse state
    ButtonNavigator buttonNavigator;
    int browseIndex = 0;
    Target* browseList[50] = {};
    int browseCount = 0;
    int detailIndex = 0;
    TargetType browseType = TargetType::AP;  // which category we're browsing
    int categoryIndex = 0;                   // 0=APs, 1=Clients, 2=BLE

    // Promiscuous callback
    static PacketRingBuffer ringBuf;
    static void IRAM_ATTR promiscCallback(void* buf, wifi_promiscuous_pkt_type_t type);

    // Phase management
    void startWifiPhase();
    void startBlePhase();
    void stopScanning();
    void processRingBuffer();

    // Render helpers
    void renderIdle() const;
    void renderDashboard() const;
    void renderCategories() const;
    void renderBrowse() const;
    void renderTargetDetail() const;

    // Utility
    void updateStats();
    void refreshBrowseList();
};
