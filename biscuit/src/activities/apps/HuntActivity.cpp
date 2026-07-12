#include "HuntActivity.h"
#include "FireActivity.h"
#include "activities/ActivityManager.h"

#include <cstring>
#include <cstdio>

#include "components/UITheme.h"
#include "fontIds.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const char* authTypeName(uint8_t authType) {
    switch (authType) {
        case 0: return "Open";
        case 1: return "WEP";
        case 2: return "WPA";
        case 3: return "WPA2-PSK";
        case 4: return "WPA3";
        default: return "Unknown";
    }
}

static const char* targetTypeName(TargetType type) {
    switch (type) {
        case TargetType::AP:  return "WiFi AP";
        case TargetType::STA: return "WiFi Client";
        case TargetType::BLE: return "BLE Device";
        default:              return "Unknown";
    }
}

static const char* targetTypePrefix(TargetType type) {
    switch (type) {
        case TargetType::AP:  return "[AP] ";
        case TargetType::STA: return "[CL] ";
        case TargetType::BLE: return "[BT] ";
        default:              return "[ ] ";
    }
}

static void formatMac(char* buf, size_t bufLen, const uint8_t mac[6]) {
    snprintf(buf, bufLen, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Format epoch seconds as HH:MM (no RTC available so just show raw seconds
// modulo a day for relative times).
static void formatTime(char* buf, size_t bufLen, uint32_t epoch) {
    if (epoch == 0) {
        snprintf(buf, bufLen, "--:--");
        return;
    }
    uint32_t sec = epoch % 86400u;
    unsigned h = sec / 3600u;
    unsigned m = (sec % 3600u) / 60u;
    snprintf(buf, bufLen, "%02u:%02u", h, m);
}

// ---------------------------------------------------------------------------
// setTarget — call before onEnter
// ---------------------------------------------------------------------------

void HuntActivity::setTarget(const uint8_t mac[6]) {
    memcpy(selectedMac, mac, 6);
    hasPreselected = true;
}

// ---------------------------------------------------------------------------
// loadTargetListByType — fills targetList[] from DB cache for one type
// ---------------------------------------------------------------------------

void HuntActivity::loadTargetListByType(TargetType type) {
    targetCount = 0;
    TARGETS.getSorted(type, targetList, 50, targetCount, 0);
    listIndex = 0;
}

// ---------------------------------------------------------------------------
// selectTarget
// ---------------------------------------------------------------------------

void HuntActivity::selectTarget(int index) {
    if (index < 0 || index >= targetCount || !targetList[index]) return;
    current = targetList[index];
    memcpy(selectedMac, current->mac, 6);
    profileScroll = 0;
    state = PROFILE_VIEW;
    buildClientList();
    analyzeCapabilities();
    requestUpdate();
}

// ---------------------------------------------------------------------------
// buildClientList — find STA targets associated with the current AP
// ---------------------------------------------------------------------------

void HuntActivity::buildClientList() {
    clientCount2 = 0;
    clientListIndex = 0;
    if (!current) return;
    TARGETS.forEach(TargetType::STA, [this](const Target& t) {
        if (clientCount2 < 20 && memcmp(t.bssid, current->mac, 6) == 0) {
            clientList[clientCount2++] = TARGETS.findByMac(t.mac);
        }
    });
}

// ---------------------------------------------------------------------------
// analyzeCapabilities
// ---------------------------------------------------------------------------

void HuntActivity::analyzeCapabilities() {
    capCount = 0;
    capIndex = 0;
    if (!current) return;

    auto add = [this](const char* n, const char* r, bool a, CapAction act = CAP_NONE, int atkType = -1) {
        if (capCount < MAX_CAPABILITIES) {
            capabilities[capCount++] = {n, r, a, act, atkType};
        }
    };

    if (current->type == TargetType::AP) {
        add("Handshake capture",
            current->pmf ? "PMF enabled — may fail" : "No PMF — feasible",
            !current->pmf, CAP_FIRE, 3);
        add("PMKID harvest",
            "Passive — no client needed",
            current->authType >= 3, CAP_FIRE, 4);
        add("Portal clone",
            "Clone AP + serve portal page",
            true, CAP_FIRE, 8);
        add("Beacon flood",
            "Flood nearby device WiFi lists",
            true, CAP_FIRE, 5);
        if (current->wps) {
            add("WPS assessment",
                "No automated tool available",
                false, CAP_NONE, -1);
        }
    } else if (current->type == TargetType::STA) {
        add("Probe response",
            current->probeCount > 0 ? "Has probed SSIDs — feasible" : "No probes seen",
            current->probeCount > 0, CAP_FIRE, 7);
        add("Track movement",
            "Monitor RSSI over time",
            true, CAP_TRACK, -1);
        add("AP association spoof",
            "Respond as known AP to client",
            current->probeCount > 0, CAP_FIRE, 7);
    } else if (current->type == TargetType::BLE) {
        add("Clone advertisement",
            "Copy BLE broadcast data",
            true, CAP_FIRE, 9);
        add("Enumerate services",
            "Connect and list GATT services",
            true, CAP_FIRE, 11);
        add("Track proximity",
            "Monitor RSSI distance",
            true, CAP_TRACK, -1);
    }
}

// ---------------------------------------------------------------------------
// onEnter / onExit
// ---------------------------------------------------------------------------

void HuntActivity::onEnter() {
    Activity::onEnter();

    if (hasPreselected) {
        current = TARGETS.findByMac(selectedMac);
        if (current) {
            state = PROFILE_VIEW;
            profileScroll = 0;
            buildClientList();
            analyzeCapabilities();
        } else {
            state = SELECT_CATEGORY;
            hasPreselected = false;
            huntCategoryIndex = 0;
        }
    } else {
        state = SELECT_CATEGORY;
        huntCategoryIndex = 0;
    }

    buttonNavigator.onNext([this] {
        if (state == SELECT_TARGET) {
            listIndex = ButtonNavigator::nextIndex(listIndex, targetCount);
            requestUpdate();
        } else if (state == CAPABILITIES) {
            capIndex = ButtonNavigator::nextIndex(capIndex, capCount);
            requestUpdate();
        } else if (state == CLIENT_LIST) {
            clientListIndex = ButtonNavigator::nextIndex(clientListIndex, clientCount2);
            requestUpdate();
        }
    });
    buttonNavigator.onPrevious([this] {
        if (state == SELECT_TARGET) {
            listIndex = ButtonNavigator::previousIndex(listIndex, targetCount);
            requestUpdate();
        } else if (state == CAPABILITIES) {
            capIndex = ButtonNavigator::previousIndex(capIndex, capCount);
            requestUpdate();
        } else if (state == CLIENT_LIST) {
            clientListIndex = ButtonNavigator::previousIndex(clientListIndex, clientCount2);
            requestUpdate();
        }
    });

    requestUpdate();
}

void HuntActivity::onExit() {
    Activity::onExit();
}

// ---------------------------------------------------------------------------
// loop
// ---------------------------------------------------------------------------

void HuntActivity::loop() {
    switch (state) {

        case SELECT_CATEGORY: {
            if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
                finish();
                return;
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                mappedInput.wasReleased(MappedInputManager::Button::Right)) {
                huntCategoryIndex = ButtonNavigator::nextIndex(huntCategoryIndex, 3);
                requestUpdate();
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                mappedInput.wasReleased(MappedInputManager::Button::Left)) {
                huntCategoryIndex = ButtonNavigator::previousIndex(huntCategoryIndex, 3);
                requestUpdate();
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
                if (huntCategoryIndex == 0)      browseType = TargetType::AP;
                else if (huntCategoryIndex == 1) browseType = TargetType::STA;
                else                             browseType = TargetType::BLE;
                loadTargetListByType(browseType);
                state = SELECT_TARGET;
                requestUpdate();
            }
            break;
        }

        case SELECT_TARGET: {
            if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
                state = SELECT_CATEGORY;
                requestUpdate();
                return;
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                mappedInput.wasReleased(MappedInputManager::Button::Right)) {
                if (targetCount > 0) {
                    listIndex = ButtonNavigator::nextIndex(listIndex, targetCount);
                    requestUpdate();
                }
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                mappedInput.wasReleased(MappedInputManager::Button::Left)) {
                if (targetCount > 0) {
                    listIndex = ButtonNavigator::previousIndex(listIndex, targetCount);
                    requestUpdate();
                }
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
                if (targetCount > 0) {
                    selectTarget(listIndex);
                }
            }
            break;
        }

        case PROFILE_VIEW: {
            if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
                if (hasPreselected) {
                    finish();
                } else {
                    state = SELECT_TARGET;
                    requestUpdate();
                }
                return;
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
                const auto& metrics   = UITheme::getInstance().getMetrics();
                const auto pageHeight = renderer.getScreenHeight();
                const int  lh         = 24;
                const int  contentTop = metrics.topPadding + metrics.headerHeight + 8;
                const int  visibleLines =
                    (pageHeight - contentTop - metrics.buttonHintsHeight) / lh;
                // 20 is a conservative upper bound on total profile lines
                const int maxScroll = (20 - visibleLines > 0) ? (20 - visibleLines) : 0;
                profileScroll++;
                if (profileScroll > maxScroll) profileScroll = maxScroll;
                requestUpdate();
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
                if (profileScroll > 0) { profileScroll--; requestUpdate(); }
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
                mappedInput.wasReleased(MappedInputManager::Button::Right)) {
                state = CAPABILITIES;
                capIndex = 0;
                requestUpdate();
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
                if (current) {
                    char path[64];
                    char macBuf[18];
                    formatMac(macBuf, sizeof(macBuf), current->mac);
                    // Replace ':' with '-' for filename safety
                    for (int i = 0; i < 17; i++) {
                        if (macBuf[i] == ':') macBuf[i] = '-';
                    }
                    snprintf(path, sizeof(path), "/biscuit/targets/%s.txt", macBuf);
                    Storage.mkdir("/biscuit");
                    Storage.mkdir("/biscuit/targets");
                    TARGETS.exportProfile(current->mac, path);
                    requestUpdate();
                }
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
                if (current && current->type == TargetType::AP && clientCount2 > 0) {
                    clientListIndex = 0;
                    state = CLIENT_LIST;
                    requestUpdate();
                }
            }
            break;
        }

        case CAPABILITIES: {
            if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
                state = PROFILE_VIEW;
                requestUpdate();
                return;
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                mappedInput.wasReleased(MappedInputManager::Button::Right)) {
                if (capCount > 0) {
                    capIndex = ButtonNavigator::nextIndex(capIndex, capCount);
                    requestUpdate();
                }
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                mappedInput.wasReleased(MappedInputManager::Button::Left)) {
                if (capCount > 0) {
                    capIndex = ButtonNavigator::previousIndex(capIndex, capCount);
                    requestUpdate();
                }
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
                if (current && capIndex < capCount && capabilities[capIndex].available) {
                    const auto& cap = capabilities[capIndex];
                    if (cap.action == CAP_FIRE && cap.fireAttackType >= 0) {
                        auto fire = std::make_unique<FireActivity>(renderer, mappedInput);
                        fire->setTarget(current->mac);
                        fire->setAttack(cap.fireAttackType);
                        activityManager.pushActivity(std::move(fire));
                    }
                    // CAP_TRACK: not implemented, ignore
                }
            }
            break;
        }

        case CLIENT_LIST: {
            if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
                state = PROFILE_VIEW;
                requestUpdate();
                return;
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                mappedInput.wasReleased(MappedInputManager::Button::Right)) {
                if (clientCount2 > 0) {
                    clientListIndex = ButtonNavigator::nextIndex(clientListIndex, clientCount2);
                    requestUpdate();
                }
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                mappedInput.wasReleased(MappedInputManager::Button::Left)) {
                if (clientCount2 > 0) {
                    clientListIndex = ButtonNavigator::previousIndex(clientListIndex, clientCount2);
                    requestUpdate();
                }
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
                if (clientCount2 > 0 && clientList[clientListIndex]) {
                    current = clientList[clientListIndex];
                    memcpy(selectedMac, current->mac, 6);
                    profileScroll = 0;
                    buildClientList();  // will be empty for a STA — that's fine
                    analyzeCapabilities();
                    state = PROFILE_VIEW;
                    requestUpdate();
                }
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------

void HuntActivity::render(RenderLock&&) {
    renderer.clearScreen();

    switch (state) {
        case SELECT_CATEGORY: renderSelectCategory(); break;
        case SELECT_TARGET:   renderSelectTarget();   break;
        case PROFILE_VIEW:    renderProfile();        break;
        case CAPABILITIES:    renderCapabilities();   break;
        case CLIENT_LIST:     renderClientList();     break;
    }

    renderer.displayBuffer();
}

// ---------------------------------------------------------------------------
// renderSelectCategory
// ---------------------------------------------------------------------------

void HuntActivity::renderSelectCategory() const {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth  = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();

    GUI.drawHeader(renderer,
        Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
        "HUNT — Select Category");

    const int listTop    = metrics.topPadding + metrics.headerHeight;
    const int hintsH     = metrics.buttonHintsHeight;
    const int listHeight = pageHeight - listTop - hintsH;

    const int apCount  = TARGETS.countByType(TargetType::AP);
    const int staCount = TARGETS.countByType(TargetType::STA);
    const int bleCount = TARGETS.countByType(TargetType::BLE);

    GUI.drawList(
        renderer,
        Rect{0, listTop, pageWidth, listHeight},
        3,
        huntCategoryIndex,
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
}

// ---------------------------------------------------------------------------
// renderSelectTarget
// ---------------------------------------------------------------------------

void HuntActivity::renderSelectTarget() const {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth  = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();

    const char* catName = (browseType == TargetType::AP)  ? "WiFi APs"
                        : (browseType == TargetType::STA) ? "Clients"
                                                          : "BLE";
    char headerTitle[32];
    snprintf(headerTitle, sizeof(headerTitle), "HUNT — %s", catName);

    GUI.drawHeader(renderer,
        Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
        headerTitle);

    const int headerBottom = metrics.topPadding + metrics.headerHeight;
    const int listHeight   = pageHeight - headerBottom - metrics.buttonHintsHeight;

    if (targetCount == 0) {
        renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, "No targets in database");
    } else {
        GUI.drawList(renderer,
            Rect{0, headerBottom, pageWidth, listHeight},
            targetCount,
            listIndex,
            [this](int i) -> std::string {
                if (!targetList[i]) return "---";
                char buf[64];
                char macBuf[18];
                formatMac(macBuf, sizeof(macBuf), targetList[i]->mac);
                const char* label = (targetList[i]->ssid[0] != '\0') ? targetList[i]->ssid
                                  : (targetList[i]->name[0] != '\0') ? targetList[i]->name
                                  : macBuf;
                snprintf(buf, sizeof(buf), "%s%s", targetTypePrefix(targetList[i]->type), label);
                return std::string(buf);
            },
            [this](int i) -> std::string {
                if (!targetList[i]) return "";
                char buf[40];
                char macBuf[18];
                formatMac(macBuf, sizeof(macBuf), targetList[i]->mac);
                snprintf(buf, sizeof(buf), "%s  RSSI %d", macBuf, (int)targetList[i]->rssi);
                return std::string(buf);
            });
    }

    const auto labels = mappedInput.mapLabels("Back", "Select", "^", "v");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ---------------------------------------------------------------------------
// renderProfile
// ---------------------------------------------------------------------------

void HuntActivity::renderProfile() const {
    if (!current) return;

    const auto& metrics  = UITheme::getInstance().getMetrics();
    const auto pageWidth = renderer.getScreenWidth();
    (void)renderer.getScreenHeight(); // pageHeight unused in profile view

    GUI.drawHeader(renderer,
        Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
        "HUNT — Profile");

    const int x  = metrics.contentSidePadding;
    const int lh = 24;  // line height
    int y = metrics.topPadding + metrics.headerHeight + 8 - profileScroll * lh;

    char macBuf[18];
    formatMac(macBuf, sizeof(macBuf), current->mac);

    // --- MAC ---
    char line[80];
    snprintf(line, sizeof(line), "MAC:  %s", macBuf);
    renderer.drawText(UI_10_FONT_ID, x, y, line); y += lh;

    // --- Type ---
    snprintf(line, sizeof(line), "Type: %s", targetTypeName(current->type));
    renderer.drawText(UI_10_FONT_ID, x, y, line); y += lh;

    // --- Vendor ---
    if (current->vendor[0] != '\0') {
        snprintf(line, sizeof(line), "Vendor: %s", current->vendor);
        renderer.drawText(UI_10_FONT_ID, x, y, line); y += lh;
    }

    // --- Type-specific fields ---
    if (current->type == TargetType::AP) {
        if (current->ssid[0] != '\0') {
            snprintf(line, sizeof(line), "SSID: \"%s\"", current->ssid);
            renderer.drawText(UI_10_FONT_ID, x, y, line); y += lh;
        }
        snprintf(line, sizeof(line), "Channel: %d   Auth: %s",
                 (int)current->channel, authTypeName(current->authType));
        renderer.drawText(UI_10_FONT_ID, x, y, line); y += lh;

        snprintf(line, sizeof(line), "PMF: %s   WPS: %s",
                 current->pmf ? "Yes" : "No",
                 current->wps ? "Yes" : "No");
        renderer.drawText(UI_10_FONT_ID, x, y, line); y += lh;

        if (clientCount2 > 0) {
            snprintf(line, sizeof(line), "Clients: %d  (PAGE BACK to view)", clientCount2);
        } else {
            snprintf(line, sizeof(line), "Clients: %d", (int)current->clientCount);
        }
        renderer.drawText(UI_10_FONT_ID, x, y, line); y += lh;

        if (current->hasHandshake || current->hasPmkid) {
            snprintf(line, sizeof(line), "Captured: %s%s",
                     current->hasHandshake ? "EAPOL " : "",
                     current->hasPmkid    ? "PMKID"  : "");
            renderer.drawText(UI_10_FONT_ID, x, y, line); y += lh;
        }

    } else if (current->type == TargetType::STA) {
        if (current->ssid[0] != '\0') {
            snprintf(line, sizeof(line), "Assoc AP: \"%s\"", current->ssid);
            renderer.drawText(UI_10_FONT_ID, x, y, line); y += lh;
        }
        snprintf(line, sizeof(line), "Probed SSIDs: %d", (int)current->probeCount);
        renderer.drawText(UI_10_FONT_ID, x, y, line); y += lh;

        for (int i = 0; i < current->probeCount && i < 5; i++) {
            snprintf(line, sizeof(line), "  [%d] \"%s\"", i + 1, current->probes[i]);
            renderer.drawText(UI_10_FONT_ID, x, y, line); y += lh;
        }

    } else if (current->type == TargetType::BLE) {
        if (current->name[0] != '\0') {
            snprintf(line, sizeof(line), "Name: \"%s\"", current->name);
            renderer.drawText(UI_10_FONT_ID, x, y, line); y += lh;
        }
    }

    // --- Common fields ---
    snprintf(line, sizeof(line), "RSSI: %d dBm", (int)current->rssi);
    renderer.drawText(UI_10_FONT_ID, x, y, line); y += lh;

    if (current->os[0] != '\0') {
        snprintf(line, sizeof(line), "OS: %s", current->os);
    } else {
        snprintf(line, sizeof(line), "OS: (unknown)");
    }
    renderer.drawText(UI_10_FONT_ID, x, y, line); y += lh;

    // --- Timestamps ---
    char tfirst[8], tlast[8];
    formatTime(tfirst, sizeof(tfirst), current->firstSeen);
    formatTime(tlast,  sizeof(tlast),  current->lastSeen);
    snprintf(line, sizeof(line), "First: %s   Last: %s", tfirst, tlast);
    renderer.drawText(UI_10_FONT_ID, x, y, line); y += lh;

    // --- Scroll hint ---
    if (profileScroll > 0) {
        renderer.drawText(SMALL_FONT_ID, x, metrics.topPadding + metrics.headerHeight + 2,
                          "^ scroll up");
    }

    // Button hints
    const auto labels = mappedInput.mapLabels("Back", "Assess", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    GUI.drawSideButtonHints(renderer, "^", "v");
}

// ---------------------------------------------------------------------------
// renderCapabilities
// ---------------------------------------------------------------------------

void HuntActivity::renderCapabilities() const {
    const auto& metrics   = UITheme::getInstance().getMetrics();
    const auto pageWidth  = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();

    GUI.drawHeader(renderer,
        Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
        "HUNT — Assessment");

    const int headerBottom = metrics.topPadding + metrics.headerHeight;
    const int listHeight   = pageHeight - headerBottom - metrics.buttonHintsHeight;

    if (capCount == 0) {
        renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, "No capabilities assessed");
    } else {
        GUI.drawList(renderer,
            Rect{0, headerBottom, pageWidth, listHeight},
            capCount,
            capIndex,
            [this](int i) -> std::string {
                char buf[64];
                snprintf(buf, sizeof(buf), "%s  [%s]",
                         capabilities[i].name,
                         capabilities[i].available ? "available" : "limited");
                return std::string(buf);
            },
            [this](int i) -> std::string {
                return std::string(capabilities[i].reason);
            });
    }

    const auto labels = mappedInput.mapLabels("Back", "Launch", "^", "v");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ---------------------------------------------------------------------------
// renderClientList
// ---------------------------------------------------------------------------

void HuntActivity::renderClientList() const {
    const auto& metrics   = UITheme::getInstance().getMetrics();
    const auto pageWidth  = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();

    GUI.drawHeader(renderer,
        Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
        "HUNT — AP Clients");

    const int headerBottom = metrics.topPadding + metrics.headerHeight;
    const int listHeight   = pageHeight - headerBottom - metrics.buttonHintsHeight;

    if (clientCount2 == 0) {
        renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, "No associated clients found");
    } else {
        GUI.drawList(renderer,
            Rect{0, headerBottom, pageWidth, listHeight},
            clientCount2,
            clientListIndex,
            [this](int i) -> std::string {
                if (!clientList[i]) return "---";
                char buf[56];
                char macBuf[18];
                formatMac(macBuf, sizeof(macBuf), clientList[i]->mac);
                if (clientList[i]->name[0] != '\0') {
                    snprintf(buf, sizeof(buf), "[CL] %s", clientList[i]->name);
                } else {
                    snprintf(buf, sizeof(buf), "[CL] %s", macBuf);
                }
                return std::string(buf);
            },
            [this](int i) -> std::string {
                if (!clientList[i]) return "";
                char buf[48];
                char macBuf[18];
                formatMac(macBuf, sizeof(macBuf), clientList[i]->mac);
                snprintf(buf, sizeof(buf), "%s  RSSI %d", macBuf, (int)clientList[i]->rssi);
                return std::string(buf);
            });
    }

    const auto labels = mappedInput.mapLabels("Back", "Select", "^", "v");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
