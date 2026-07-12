#pragma once
#include <cstdint>
#include <cstring>
#include <functional>

enum class TargetType : uint8_t {
    AP,
    STA,
    BLE
};

struct Target {
    uint8_t mac[6] = {};
    TargetType type = TargetType::AP;
    char ssid[33] = {};           // for APs: SSID. For clients: connected AP SSID
    uint8_t bssid[6] = {};        // for clients: associated AP BSSID
    char name[33] = {};           // BLE device name or friendly name
    char vendor[24] = {};         // OUI vendor string
    char os[16] = {};             // fingerprinted OS
    int8_t rssi = -127;           // last seen RSSI
    uint8_t channel = 0;          // WiFi channel
    uint8_t authType = 0;         // 0=open, 1=WEP, 2=WPA, 3=WPA2, 4=WPA3
    bool pmf = false;             // Protected Management Frames
    bool wps = false;             // WPS enabled
    uint32_t firstSeen = 0;       // epoch seconds
    uint32_t lastSeen = 0;        // epoch seconds
    uint8_t clientCount = 0;      // for APs: associated clients
    char probes[5][33] = {};      // for clients: probed SSIDs (max 5)
    uint8_t probeCount = 0;
    uint8_t bleAssocMac[6] = {};  // correlated BLE/WiFi MAC
    bool hasHandshake = false;    // EAPOL captured
    bool hasPmkid = false;        // PMKID captured

    bool macEquals(const uint8_t other[6]) const {
        return memcmp(mac, other, 6) == 0;
    }
};

class TargetDB {
public:
    static TargetDB& instance();

    // Core operations
    Target* findByMac(const uint8_t mac[6]);
    void addOrUpdate(const Target& t);
    void remove(const uint8_t mac[6]);

    // Queries
    int countByType(TargetType type) const;
    int totalCached() const { return cacheCount; }
    void forEach(TargetType type, const std::function<void(const Target&)>& fn) const;

    // Sorted access for UI — caller provides output buffer
    // sortBy: 0=lastSeen desc, 1=rssi desc, 2=firstSeen asc
    void getSorted(TargetType type, Target** out, int maxOut, int& outCount, int sortBy = 0);

    // Lifecycle
    void loadCache();     // read from SD into cache
    void flush();         // force write pending changes to SD
    void clear();         // wipe all data

    // Export
    bool exportProfile(const uint8_t mac[6], const char* path);

    // Session tracking
    bool isSeenThisSession(const Target* t) const;

private:
    TargetDB() = default;
    static constexpr int MAX_CACHE = 50;
    Target cache[MAX_CACHE] = {};
    int cacheCount = 0;
    bool dirty = false;
    bool seenThisSession[MAX_CACHE] = {};

    int findCacheIndex(const uint8_t mac[6]) const;
    void evictOldest();
    void appendToSD(const Target& t);
    void rewriteSD();
    void loadFromSD();
};

#define TARGETS TargetDB::instance()
