#pragma once
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class LootActivity final : public Activity {
public:
    explicit LootActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
        : Activity("Loot", renderer, mappedInput) {}

    void onEnter() override;
    void onExit() override;
    void loop() override;
    void render(RenderLock&&) override;

private:
    enum State { OVERVIEW, HANDSHAKES, CREDENTIALS, PCAP_FILES, BLE_DUMPS, ITEM_DETAIL };
    enum DetailAction { VIEW, EXPORT, DELETE };

    State state = OVERVIEW;
    ButtonNavigator buttonNavigator;

    // Overview counts
    int handshakeCount = 0;
    int credentialCount = 0;
    int pcapCount = 0;
    int bleDumpCount = 0;

    // Category selector in OVERVIEW
    int overviewIndex = 0;
    static constexpr int CATEGORY_COUNT = 4;

    // File lists (store filenames, max 20 per category)
    static constexpr int MAX_FILES = 20;
    char fileNames[MAX_FILES][32] = {};
    int fileCount = 0;
    int fileIndex = 0;

    // Detail state
    char detailBuf[256] = {};
    int detailScroll = 0;
    DetailAction detailAction = VIEW;

    // Credential parsing
    struct Credential {
        char ssid[33];
        char username[33];
        char password[33];
        char timestamp[20];
    };
    Credential creds[MAX_FILES] = {};
    int credCount = 0;

    // Tracks which category's file list is currently loaded
    State listCategory = HANDSHAKES;

    void countFiles();
    void loadHandshakes();
    void loadCredentials();
    void loadPcapFiles();
    void loadBleDumps();
    void loadFileDetail(int index);
    void exportSummaryReport();
    void executeDetailAction();

    void renderOverview() const;
    void renderFileList(const char* title) const;
    void renderCredentialList() const;
    void renderDetail() const;
};
