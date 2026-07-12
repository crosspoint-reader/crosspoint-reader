#pragma once
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "util/TargetDB.h"
#include "util/PacketRingBuffer.h"
#include <esp_wifi.h>

class FireActivity final : public Activity {
public:
    explicit FireActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
        : Activity("Fire", renderer, mappedInput) {}

    void setTarget(const uint8_t mac[6]);
    void setAttack(int attackType);

    void onEnter() override;
    void onExit() override;
    void loop() override;
    void render(RenderLock&&) override;
    bool preventAutoSleep() override { return state == EXECUTING; }
    bool skipLoopDelay() override { return state == EXECUTING; }

private:
    // --- States ---
    enum State { TARGET_SELECT, ATTACK_SELECT, CONFIRM, EXECUTING, RESULTS };
    enum AttackType {
        ATK_DEAUTH_BROADCAST,
        ATK_DEAUTH_TARGETED,
        ATK_ROGUE_AP,
        ATK_HANDSHAKE_CAPTURE,
        ATK_PMKID_HARVEST,
        ATK_BEACON_FLOOD,
        ATK_AUTH_FLOOD,
        ATK_KARMA_AP,
        ATK_EVIL_TWIN,
        ATK_BLE_CLONE,
        ATK_BLE_SPAM,
        ATK_BLE_ENUMERATE,
        ATK_AIRTAG_SWARM,
        ATK_COUNT
    };

    struct AttackEntry {
        AttackType type;
        const char* name;
        const char* desc;
        bool available;
    };

    State state = TARGET_SELECT;
    ButtonNavigator buttonNavigator;

    // Target
    uint8_t targetMac[6] = {};
    Target* target = nullptr;
    bool hasPreselected = false;
    int preselectedAttack = -1;
    AttackType pendingAttack = ATK_COUNT;  // which attack triggered TARGET_SELECT

    // Target select list
    Target* targetList[50] = {};
    int targetCount = 0;
    int targetIndex = 0;

    // Attack menu
    AttackEntry attacks[ATK_COUNT] = {};
    int availableCount = 0;
    int attackIndex = 0;

    // Execution
    AttackType activeAttack = ATK_DEAUTH_BROADCAST;
    unsigned long attackStartMs = 0;
    unsigned long lastActionMs = 0;
    unsigned long lastDisplayMs = 0;
    uint32_t packetsSent = 0;
    char statusLine[80] = {};
    char resultLine[128] = {};

    // EAPOL/PMKID capture
    int eapolCount = 0;
    bool pmkidFound = false;
    bool captureComplete = false;

    // Beacon flood
    int beaconCount = 0;
    static constexpr int FLOOD_SSID_COUNT = 30;
    char floodSSIDs[FLOOD_SSID_COUNT][33] = {};

    // Karma
    int karmaIndex = 0;

    // Promiscuous capture buffer (separate from ScanActivity's)
    static PacketRingBuffer captureBuf;
    static void IRAM_ATTR captureCallback(void* buf, wifi_promiscuous_pkt_type_t type);

    // Build
    void buildAttackMenu();
    void loadTargetList();
    static bool isUniversalAttack(AttackType atk);
    TargetType requiredTargetType(AttackType atk) const;

    // Attack execution (called repeatedly from loop)
    void tickDeauthBroadcast();
    void tickDeauthTargeted();
    void tickRogueAp();
    void tickHandshakeCapture();
    void tickPmkidHarvest();
    void tickBeaconFlood();
    void tickAuthFlood();
    void tickKarmaAp();
    void tickEvilTwin();
    void tickBleClone();
    void tickBleSpam();
    void tickBleEnumerate();
    void tickAirtagSwarm();

    // Attack start/stop
    void startAttack();
    void stopAttack();

    // EAPOL processing
    void processCaptureBuf();
    void checkEapolFrame(const uint8_t* data, uint16_t len);

    // PCAP writing
    void savePcapHeader(const char* path);
    void appendPcapPacket(const char* path, const uint8_t* data, uint16_t len);

    // Helpers
    void randomMAC(uint8_t mac[6]);
    void buildDeauthFrame(uint8_t* frame, const uint8_t* bssid, const uint8_t* client);
    void buildBeaconFrame(uint8_t* frame, int& len, const char* ssid, const uint8_t* bssid, uint8_t channel);
    void buildAuthFrame(uint8_t* frame, const uint8_t* bssid, const uint8_t* src);

    // Render
    void renderTargetSelect() const;
    void renderAttackSelect() const;
    void renderConfirm() const;
    void renderExecuting() const;
    void renderResults() const;
};
