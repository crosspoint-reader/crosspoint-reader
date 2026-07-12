#pragma once
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct GhostConfig {
    bool macRotateEnabled = false;
    uint32_t macRotateIntervalMs = 60000;  // 1 min default
    int8_t txPower = 84;                    // max (20dBm) default
    bool probeSuppress = false;
    bool deadManEnabled = false;
    uint16_t deadManMinutes = 30;
};

class GhostActivity final : public Activity {
public:
    explicit GhostActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
        : Activity("Ghost", renderer, mappedInput) {}

    void onEnter() override;
    void onExit() override;
    void loop() override;
    void render(RenderLock&&) override;
    bool preventAutoSleep() override { return true; }

private:
    enum State { DASHBOARD, CONFIG_VIEW, CONFIRM_WIPE, CONFIRM_RF_KILL };

    State state = DASHBOARD;
    GhostConfig config;
    ButtonNavigator buttonNavigator;

    // Dashboard
    int dashAction = 0;  // 0=RF Kill, 1=Quick Wipe, 2=Settings
    static constexpr int DASH_ACTION_COUNT = 3;

    // Settings
    int settingIndex = 0;
    static constexpr int SETTING_COUNT = 6;
    // Settings: MAC Rotate On/Off, Rotate Interval, TX Power, Probe Suppress, Dead Man On/Off, Dead Man Timer

    // MAC rotation state
    unsigned long lastMacRotate = 0;
    uint8_t currentMac[6] = {};
    bool macRotationActive = false;

    // Dead man's switch
    unsigned long lastButtonPress = 0;

    // Display throttle
    unsigned long lastDisplayMs = 0;
    static constexpr unsigned long DISPLAY_INTERVAL_MS = 1000;

    // Status strings
    char macStr[18] = {};
    char rotateCountdown[16] = {};

    void loadConfig();
    void saveConfig();
    void rotateMac();
    void executeRfKill();
    void executeQuickWipe();
    void checkDeadManSwitch();
    void updateMacString();

    void renderDashboard() const;
    void renderSettings() const;
    void renderConfirmWipe() const;
    void renderConfirmRfKill() const;
};
