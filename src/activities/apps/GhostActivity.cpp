#include "GhostActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_wifi.h>

#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"
#include "util/TargetDB.h"

static constexpr char kModule[] = "GHOST";
static constexpr char kConfigPath[] = "/biscuit/ghost.dat";

// ---------------------------------------------------------------------------
// Config persistence
// ---------------------------------------------------------------------------

void GhostActivity::loadConfig() {
    config = GhostConfig{};
    FsFile file;
    if (Storage.openFileForRead(kModule, kConfigPath, file)) {
        file.read(reinterpret_cast<uint8_t*>(&config), sizeof(GhostConfig));
        file.close();
    }
}

void GhostActivity::saveConfig() {
    Storage.mkdir("/biscuit");
    FsFile file;
    if (Storage.openFileForWrite(kModule, kConfigPath, file)) {
        file.write(reinterpret_cast<const uint8_t*>(&config), sizeof(GhostConfig));
        file.close();
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void GhostActivity::onEnter() {
    Activity::onEnter();
    loadConfig();
    lastButtonPress = millis();

    // Read current MAC (no radio bring-up needed — reads eFuse / driver state)
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        memcpy(currentMac, mac, 6);
    } else {
        // Radio not initialised yet — read from eFuse base address
        esp_read_mac(currentMac, ESP_MAC_WIFI_STA);
    }
    updateMacString();

    state = DASHBOARD;
    requestUpdate();
}

void GhostActivity::onExit() {
    saveConfig();
    Activity::onExit();
}

// ---------------------------------------------------------------------------
// MAC helpers
// ---------------------------------------------------------------------------

void GhostActivity::updateMacString() {
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             currentMac[0], currentMac[1], currentMac[2],
             currentMac[3], currentMac[4], currentMac[5]);
}

void GhostActivity::rotateMac() {
    uint8_t newMac[6];
    esp_fill_random(newMac, 6);
    // Locally administered, unicast
    newMac[0] = (newMac[0] & 0xFC) | 0x02;

    // Only attempt if WiFi radio is actually running
    if (RADIO.getState() == RadioManager::RadioState::WIFI) {
        esp_wifi_set_mac(WIFI_IF_STA, newMac);
    }

    memcpy(currentMac, newMac, 6);
    updateMacString();
    lastMacRotate = millis();
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void GhostActivity::executeRfKill() {
    RADIO.shutdown();
    macRotationActive = false;
}

void GhostActivity::executeQuickWipe() {
    TARGETS.clear();
    Storage.remove("/biscuit/creds.csv");
    Storage.remove("/biscuit/targets.dat");
    Storage.remove("/biscuit/ghost.dat");
    config = GhostConfig{};
}

void GhostActivity::checkDeadManSwitch() {
    unsigned long elapsed = millis() - lastButtonPress;
    unsigned long timeout = static_cast<unsigned long>(config.deadManMinutes) * 60000UL;
    if (elapsed >= timeout) {
        executeQuickWipe();
        executeRfKill();
        // Reset timer so we don't fire repeatedly every loop tick
        lastButtonPress = millis();
    }
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------

void GhostActivity::loop() {
    // Reset dead man's switch on any button press
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
        mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Down) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left) ||
        mappedInput.wasPressed(MappedInputManager::Button::Right) ||
        mappedInput.wasPressed(MappedInputManager::Button::PageForward)) {
        lastButtonPress = millis();
    }

    // MAC rotation
    if (config.macRotateEnabled && macRotationActive) {
        if (millis() - lastMacRotate >= config.macRotateIntervalMs) {
            rotateMac();
        }
    }

    // Dead man's switch check
    if (config.deadManEnabled) {
        checkDeadManSwitch();
    }

    // Display throttle: refresh countdown timers once per second
    if (millis() - lastDisplayMs >= DISPLAY_INTERVAL_MS) {
        lastDisplayMs = millis();
        requestUpdate();
    }

    // State-specific input handling
    switch (state) {
        case DASHBOARD: {
            if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
                dashAction = (dashAction - 1 + DASH_ACTION_COUNT) % DASH_ACTION_COUNT;
                requestUpdate();
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
                dashAction = (dashAction + 1) % DASH_ACTION_COUNT;
                requestUpdate();
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
                switch (dashAction) {
                    case 0: state = CONFIRM_RF_KILL; break;
                    case 1: state = CONFIRM_WIPE;    break;
                    case 2: state = CONFIG_VIEW; settingIndex = 0; break;
                }
                requestUpdate();
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
                finish();
            }
            break;
        }

        case CONFIG_VIEW: {
            if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
                settingIndex = (settingIndex - 1 + SETTING_COUNT) % SETTING_COUNT;
                requestUpdate();
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
                settingIndex = (settingIndex + 1) % SETTING_COUNT;
                requestUpdate();
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
                switch (settingIndex) {
                    case 0:
                        config.macRotateEnabled = !config.macRotateEnabled;
                        if (config.macRotateEnabled) {
                            macRotationActive = true;
                            lastMacRotate = millis();
                        } else {
                            macRotationActive = false;
                        }
                        break;
                    case 1:
                        // Cycle interval: 30s -> 60s -> 120s -> 300s -> 30s
                        if (config.macRotateIntervalMs <= 30000)       config.macRotateIntervalMs = 60000;
                        else if (config.macRotateIntervalMs <= 60000)  config.macRotateIntervalMs = 120000;
                        else if (config.macRotateIntervalMs <= 120000) config.macRotateIntervalMs = 300000;
                        else                                            config.macRotateIntervalMs = 30000;
                        break;
                    case 2:
                        // Cycle TX power: 8 (2dBm stealth) -> 44 (11dBm medium) -> 84 (20dBm max) -> 8
                        if (config.txPower <= 8)       config.txPower = 44;
                        else if (config.txPower <= 44) config.txPower = 84;
                        else                           config.txPower = 8;
                        break;
                    case 3:
                        config.probeSuppress = !config.probeSuppress;
                        break;
                    case 4:
                        config.deadManEnabled = !config.deadManEnabled;
                        if (config.deadManEnabled) lastButtonPress = millis();
                        break;
                    case 5:
                        // Cycle dead man timer: 5 -> 15 -> 30 -> 60 -> 5 minutes
                        if (config.deadManMinutes <= 5)       config.deadManMinutes = 15;
                        else if (config.deadManMinutes <= 15) config.deadManMinutes = 30;
                        else if (config.deadManMinutes <= 30) config.deadManMinutes = 60;
                        else                                  config.deadManMinutes = 5;
                        break;
                }
                saveConfig();
                requestUpdate();
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
                state = DASHBOARD;
                requestUpdate();
            }
            break;
        }

        case CONFIRM_RF_KILL: {
            if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
                executeRfKill();
                state = DASHBOARD;
                requestUpdate();
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
                state = DASHBOARD;
                requestUpdate();
            }
            break;
        }

        case CONFIRM_WIPE: {
            if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
                executeQuickWipe();
                state = DASHBOARD;
                requestUpdate();
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
                state = DASHBOARD;
                requestUpdate();
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void GhostActivity::render(RenderLock&&) {
    renderer.clearScreen();
    switch (state) {
        case DASHBOARD:       renderDashboard();    break;
        case CONFIG_VIEW:        renderSettings();     break;
        case CONFIRM_WIPE:    renderConfirmWipe();  break;
        case CONFIRM_RF_KILL: renderConfirmRfKill(); break;
    }
    renderer.displayBuffer();
}

// ---------------------------------------------------------------------------
// renderDashboard
// ---------------------------------------------------------------------------

void GhostActivity::renderDashboard() const {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth = renderer.getScreenWidth();

    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Ghost - Stealth Mode");

    const int leftPad   = metrics.contentSidePadding;
    const int lineH     = renderer.getLineHeight(UI_10_FONT_ID) + 6;
    int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

    // MAC address line
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "MAC: %s", macStr);
        renderer.drawText(UI_10_FONT_ID, leftPad, y, buf, true);
        y += lineH;
    }

    // MAC rotation countdown
    {
        char buf[40];
        if (config.macRotateEnabled && macRotationActive) {
            unsigned long elapsed = millis() - lastMacRotate;
            unsigned long remaining = (elapsed < config.macRotateIntervalMs)
                                      ? (config.macRotateIntervalMs - elapsed) / 1000
                                      : 0;
            snprintf(buf, sizeof(buf), "Rotate: ON  (next: %lus)", remaining);
        } else {
            snprintf(buf, sizeof(buf), "Rotate: %s", config.macRotateEnabled ? "ON  (inactive)" : "OFF");
        }
        renderer.drawText(UI_10_FONT_ID, leftPad, y, buf, true);
        y += lineH;
    }

    // TX Power
    {
        const char* pwrLabel;
        if (config.txPower <= 8)       pwrLabel = "LOW  (2dBm)";
        else if (config.txPower <= 44) pwrLabel = "MED  (11dBm)";
        else                           pwrLabel = "MAX  (20dBm)";

        char buf[32];
        snprintf(buf, sizeof(buf), "TX Power: %s", pwrLabel);
        renderer.drawText(UI_10_FONT_ID, leftPad, y, buf, true);
        y += lineH;
    }

    // Probe suppression
    {
        renderer.drawText(UI_10_FONT_ID, leftPad, y,
                          config.probeSuppress ? "Probes: SUPPRESSED" : "Probes: NORMAL",
                          true);
        y += lineH;
    }

    // Radio state
    {
        const char* radioLabel;
        switch (RADIO.getState()) {
            case RadioManager::RadioState::WIFI: radioLabel = "Radio: WIFI"; break;
            case RadioManager::RadioState::BLE:  radioLabel = "Radio: BLE";  break;
            default:                             radioLabel = "Radio: OFF";  break;
        }
        renderer.drawText(UI_10_FONT_ID, leftPad, y, radioLabel, true);
        y += lineH + 4;
    }

    // Dead man's switch
    {
        char buf[48];
        if (config.deadManEnabled) {
            unsigned long elapsed = millis() - lastButtonPress;
            unsigned long timeout = static_cast<unsigned long>(config.deadManMinutes) * 60000UL;
            unsigned long remainSec = (elapsed < timeout) ? (timeout - elapsed) / 1000 : 0;
            unsigned long remMin  = remainSec / 60;
            unsigned long remSec2 = remainSec % 60;
            snprintf(buf, sizeof(buf), "Dead Man: ON  (%02lu:%02lu left)", remMin, remSec2);
        } else {
            snprintf(buf, sizeof(buf), "Dead Man: OFF");
        }
        renderer.drawText(UI_10_FONT_ID, leftPad, y, buf, true);
        y += lineH + 8;
    }

    // Divider before action list
    renderer.drawLine(leftPad, y, pageWidth - leftPad, y, true);
    y += 6;

    // Action list: 0=RF Kill, 1=Quick Wipe, 2=Settings
    static const char* const kActions[DASH_ACTION_COUNT] = {
        "[RF Kill]",
        "[Quick Wipe]",
        "[Settings]",
    };

    for (int i = 0; i < DASH_ACTION_COUNT; i++) {
        const bool selected = (i == dashAction);
        if (selected) {
            renderer.fillRect(leftPad - 4, y - 2,
                              pageWidth - (leftPad - 4) * 2,
                              renderer.getLineHeight(UI_10_FONT_ID) + 4,
                              true);
        }
        char row[32];
        snprintf(row, sizeof(row), "%s %s", selected ? ">" : " ", kActions[i]);
        renderer.drawText(UI_10_FONT_ID, leftPad, y, row, !selected);
        y += lineH;
    }

    // Button hints
    const auto labels = mappedInput.mapLabels("Back", "OK", "^", "v");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ---------------------------------------------------------------------------
// renderSettings
// ---------------------------------------------------------------------------

void GhostActivity::renderSettings() const {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth = renderer.getScreenWidth();

    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Ghost - Settings");

    // Build label+value strings for each setting (no heap: stack buffers)
    char labels[SETTING_COUNT][48];

    // 0: MAC Rotation
    snprintf(labels[0], sizeof(labels[0]), "MAC Rotation:     %s",
             config.macRotateEnabled ? "ON" : "OFF");

    // 1: Rotate Interval
    {
        uint32_t s = config.macRotateIntervalMs / 1000;
        snprintf(labels[1], sizeof(labels[1]), "Rotate Interval:  %lus",
                 static_cast<unsigned long>(s));
    }

    // 2: TX Power
    {
        const char* pwrLabel;
        if (config.txPower <= 8)       pwrLabel = "LOW  (2dBm)";
        else if (config.txPower <= 44) pwrLabel = "MED  (11dBm)";
        else                           pwrLabel = "MAX  (20dBm)";
        snprintf(labels[2], sizeof(labels[2]), "TX Power:         %s", pwrLabel);
    }

    // 3: Probe Suppress
    snprintf(labels[3], sizeof(labels[3]), "Probe Suppress:   %s",
             config.probeSuppress ? "ON" : "OFF");

    // 4: Dead Man Switch
    snprintf(labels[4], sizeof(labels[4]), "Dead Man Switch:  %s",
             config.deadManEnabled ? "ON" : "OFF");

    // 5: Dead Man Timer
    snprintf(labels[5], sizeof(labels[5]), "Dead Man Timer:   %u min",
             static_cast<unsigned>(config.deadManMinutes));

    // Use drawList with prebuilt strings
    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int listH      = renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight
                           - metrics.verticalSpacing;

    GUI.drawList(
        renderer,
        Rect{0, contentTop, pageWidth, listH},
        SETTING_COUNT,
        settingIndex,
        [&labels](int i) -> std::string { return labels[i]; }
    );

    const auto btnLabels = mappedInput.mapLabels("Back", "Toggle", "^", "v");
    GUI.drawButtonHints(renderer, btnLabels.btn1, btnLabels.btn2, btnLabels.btn3, btnLabels.btn4);
}

// ---------------------------------------------------------------------------
// renderConfirmWipe / renderConfirmRfKill
// ---------------------------------------------------------------------------

void GhostActivity::renderConfirmWipe() const {
    GUI.drawPopup(renderer, "WIPE ALL DATA?\nPress OK to confirm\nBack to cancel");

    const auto labels = mappedInput.mapLabels("Cancel", "Confirm", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void GhostActivity::renderConfirmRfKill() const {
    GUI.drawPopup(renderer, "KILL ALL RADIOS?\nPress OK to confirm\nBack to cancel");

    const auto labels = mappedInput.mapLabels("Cancel", "Confirm", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
