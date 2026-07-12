#include "TargetDB.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

static constexpr const char* kTargetsPath = "/biscuit/targets.dat";
static constexpr const char* kModule = "TDB";

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

TargetDB& TargetDB::instance() {
    static TargetDB db;
    return db;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

int TargetDB::findCacheIndex(const uint8_t mac[6]) const {
    for (int i = 0; i < cacheCount; ++i) {
        if (memcmp(cache[i].mac, mac, 6) == 0) {
            return i;
        }
    }
    return -1;
}

void TargetDB::evictOldest() {
    if (cacheCount == 0) return;

    int oldest = 0;
    for (int i = 1; i < cacheCount; ++i) {
        if (cache[i].lastSeen < cache[oldest].lastSeen) {
            oldest = i;
        }
    }

    // Shift remaining entries down
    const int remaining = cacheCount - oldest - 1;
    if (remaining > 0) {
        memmove(&cache[oldest], &cache[oldest + 1], remaining * sizeof(Target));
        memmove(&seenThisSession[oldest], &seenThisSession[oldest + 1], remaining * sizeof(bool));
    }
    --cacheCount;
}

// ---------------------------------------------------------------------------
// Core operations
// ---------------------------------------------------------------------------

bool TargetDB::isSeenThisSession(const Target* t) const {
    if (!t) return false;
    int idx = static_cast<int>(t - &cache[0]);
    return (idx >= 0 && idx < cacheCount) && seenThisSession[idx];
}

Target* TargetDB::findByMac(const uint8_t mac[6]) {
    int idx = findCacheIndex(mac);
    if (idx >= 0) {
        return &cache[idx];
    }
    return nullptr;
}

void TargetDB::addOrUpdate(const Target& t) {
    int idx = findCacheIndex(t.mac);

    if (idx >= 0) {
        // Update existing entry — merge fields
        Target& existing = cache[idx];

        // Always update rssi and lastSeen
        existing.rssi = t.rssi;
        existing.lastSeen = t.lastSeen;
        seenThisSession[idx] = true;

        // Update string/value fields only if the new value is non-empty/non-zero
        if (t.ssid[0] != '\0') {
            memcpy(existing.ssid, t.ssid, sizeof(existing.ssid));
        }
        if (t.name[0] != '\0') {
            memcpy(existing.name, t.name, sizeof(existing.name));
        }
        if (t.vendor[0] != '\0') {
            memcpy(existing.vendor, t.vendor, sizeof(existing.vendor));
        }
        if (t.os[0] != '\0') {
            memcpy(existing.os, t.os, sizeof(existing.os));
        }
        if (t.channel != 0) {
            existing.channel = t.channel;
        }
        if (t.authType != 0) {
            existing.authType = t.authType;
        }
        if (t.pmf) {
            existing.pmf = true;
        }
        if (t.wps) {
            existing.wps = true;
        }
        if (t.clientCount > 0) {
            existing.clientCount = t.clientCount;
        }

        // Merge probes — add new probes not already in the list (up to 5)
        for (int pi = 0; pi < t.probeCount && t.probes[pi][0] != '\0'; ++pi) {
            bool found = false;
            for (int ei = 0; ei < existing.probeCount; ++ei) {
                if (strncmp(existing.probes[ei], t.probes[pi], 33) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found && existing.probeCount < 5) {
                memcpy(existing.probes[existing.probeCount], t.probes[pi], 33);
                ++existing.probeCount;
            }
        }

        // Never clear hasHandshake / hasPmkid once set
        if (t.hasHandshake) existing.hasHandshake = true;
        if (t.hasPmkid) existing.hasPmkid = true;

    } else {
        // New entry
        if (cacheCount >= MAX_CACHE) {
            evictOldest();
        }
        cache[cacheCount] = t;
        ++cacheCount;
        seenThisSession[cacheCount - 1] = true;
    }

    dirty = true;
}

void TargetDB::remove(const uint8_t mac[6]) {
    int idx = findCacheIndex(mac);
    if (idx < 0) return;

    const int remaining = cacheCount - idx - 1;
    if (remaining > 0) {
        memmove(&cache[idx], &cache[idx + 1], remaining * sizeof(Target));
        memmove(&seenThisSession[idx], &seenThisSession[idx + 1], remaining * sizeof(bool));
    }
    --cacheCount;
    dirty = true;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

int TargetDB::countByType(TargetType type) const {
    int count = 0;
    for (int i = 0; i < cacheCount; ++i) {
        if (cache[i].type == type) ++count;
    }
    return count;
}

void TargetDB::forEach(TargetType type, const std::function<void(const Target&)>& fn) const {
    for (int i = 0; i < cacheCount; ++i) {
        if (cache[i].type == type) {
            fn(cache[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// Sorted access — insertion sort on pointer array, no heap
// ---------------------------------------------------------------------------

void TargetDB::getSorted(TargetType type, Target** out, int maxOut, int& outCount, int sortBy) {
    outCount = 0;

    // Collect matching pointers
    for (int i = 0; i < cacheCount && outCount < maxOut; ++i) {
        if (cache[i].type == type) {
            out[outCount++] = &cache[i];
        }
    }

    // Insertion sort
    for (int i = 1; i < outCount; ++i) {
        Target* key = out[i];
        int j = i - 1;

        bool shouldSwap = false;
        while (j >= 0) {
            const Target* a = out[j];
            if (sortBy == 0) {
                // Live targets first, then lastSeen descending within each group
                int ai = static_cast<int>(a - cache);
                int ki = static_cast<int>(key - cache);
                bool aLive = (ai >= 0 && ai < cacheCount) && seenThisSession[ai];
                bool kLive = (ki >= 0 && ki < cacheCount) && seenThisSession[ki];
                if (aLive != kLive) {
                    shouldSwap = kLive && !aLive;
                } else {
                    shouldSwap = a->lastSeen < key->lastSeen;
                }
            } else if (sortBy == 1) {
                // rssi descending
                shouldSwap = a->rssi < key->rssi;
            } else {
                // firstSeen ascending
                shouldSwap = a->firstSeen > key->firstSeen;
            }

            if (!shouldSwap) break;
            out[j + 1] = out[j];
            --j;
        }
        out[j + 1] = key;
    }
}

// ---------------------------------------------------------------------------
// SD persistence
// ---------------------------------------------------------------------------

void TargetDB::loadFromSD() {
    if (!Storage.exists(kTargetsPath)) {
        LOG_DBG(kModule, "No targets file found, starting empty");
        return;
    }

    FsFile file;
    if (!Storage.openFileForRead(kModule, kTargetsPath, file)) {
        LOG_ERR(kModule, "Failed to open targets file for read");
        return;
    }

    // Read all records; keep the most-recent ones if file exceeds MAX_CACHE.
    // Strategy: read all into a temporary flat buffer of up to 2*MAX_CACHE
    // records, then select the MAX_CACHE most recent by lastSeen.
    // To avoid large stack allocation we read directly into cache and, if we
    // overflow, evict the oldest on each new record (same behaviour as live
    // addOrUpdate path but without merge logic — just blind insert).
    cacheCount = 0;

    Target t;
    while (file.read(&t, sizeof(Target)) == static_cast<int>(sizeof(Target))) {
        if (cacheCount >= MAX_CACHE) {
            evictOldest();
        }
        cache[cacheCount++] = t;
    }

    file.close();
    dirty = false;
    LOG_DBG(kModule, "Loaded %d targets from SD", cacheCount);
}

void TargetDB::loadCache() {
    cacheCount = 0;
    dirty = false;
    memset(seenThisSession, 0, sizeof(seenThisSession));
    Storage.mkdir("/biscuit");
    loadFromSD();
}

void TargetDB::rewriteSD() {
    FsFile file;
    if (!Storage.openFileForWrite(kModule, kTargetsPath, file)) {
        LOG_ERR(kModule, "Failed to open targets file for write");
        return;
    }

    for (int i = 0; i < cacheCount; ++i) {
        file.write(reinterpret_cast<const uint8_t*>(&cache[i]), sizeof(Target));
    }

    file.close();
    dirty = false;
    LOG_DBG(kModule, "Flushed %d targets to SD", cacheCount);
}

void TargetDB::flush() {
    if (!dirty) return;
    Storage.mkdir("/biscuit");
    rewriteSD();
}

void TargetDB::appendToSD(const Target& t) {
    Storage.mkdir("/biscuit");
    // Open for append: use the raw open() with O_WRITE | O_CREAT | O_APPEND
    FsFile file = Storage.open(kTargetsPath, O_WRITE | O_CREAT | O_APPEND);
    if (!file) {
        LOG_ERR(kModule, "Failed to open targets file for append");
        return;
    }
    file.write(reinterpret_cast<const uint8_t*>(&t), sizeof(Target));
    file.close();
}

void TargetDB::clear() {
    memset(cache, 0, sizeof(cache));
    memset(seenThisSession, 0, sizeof(seenThisSession));
    cacheCount = 0;
    dirty = false;

    // Truncate the file by opening for write (openFileForWrite truncates)
    if (Storage.exists(kTargetsPath)) {
        FsFile file;
        if (Storage.openFileForWrite(kModule, kTargetsPath, file)) {
            file.close();
        }
    }
    LOG_DBG(kModule, "TargetDB cleared");
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

bool TargetDB::exportProfile(const uint8_t mac[6], const char* path) {
    int idx = findCacheIndex(mac);
    if (idx < 0) return false;

    const Target& t = cache[idx];

    FsFile file;
    if (!Storage.openFileForWrite(kModule, path, file)) {
        LOG_ERR(kModule, "Failed to open export file: %s", path);
        return false;
    }

    // Human-readable text profile
    char buf[128];

    snprintf(buf, sizeof(buf), "MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
             t.mac[0], t.mac[1], t.mac[2], t.mac[3], t.mac[4], t.mac[5]);
    file.write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));

    const char* typeStr = "UNKNOWN";
    if (t.type == TargetType::AP)     typeStr = "WIFI_AP";
    else if (t.type == TargetType::STA) typeStr = "WIFI_CLIENT";
    else if (t.type == TargetType::BLE)  typeStr = "BLE_DEVICE";

    snprintf(buf, sizeof(buf), "Type: %s\n", typeStr);
    file.write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));

    if (t.ssid[0] != '\0') {
        snprintf(buf, sizeof(buf), "SSID: %s\n", t.ssid);
        file.write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));
    }
    if (t.name[0] != '\0') {
        snprintf(buf, sizeof(buf), "Name: %s\n", t.name);
        file.write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));
    }
    if (t.vendor[0] != '\0') {
        snprintf(buf, sizeof(buf), "Vendor: %s\n", t.vendor);
        file.write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));
    }
    if (t.os[0] != '\0') {
        snprintf(buf, sizeof(buf), "OS: %s\n", t.os);
        file.write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));
    }

    snprintf(buf, sizeof(buf), "RSSI: %d\n", static_cast<int>(t.rssi));
    file.write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));

    snprintf(buf, sizeof(buf), "Channel: %u\n", t.channel);
    file.write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));

    snprintf(buf, sizeof(buf), "AuthType: %u\n", t.authType);
    file.write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));

    snprintf(buf, sizeof(buf), "PMF: %s\n", t.pmf ? "yes" : "no");
    file.write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));

    snprintf(buf, sizeof(buf), "WPS: %s\n", t.wps ? "yes" : "no");
    file.write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));

    snprintf(buf, sizeof(buf), "FirstSeen: %lu\n", static_cast<unsigned long>(t.firstSeen));
    file.write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));

    snprintf(buf, sizeof(buf), "LastSeen: %lu\n", static_cast<unsigned long>(t.lastSeen));
    file.write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));

    snprintf(buf, sizeof(buf), "ClientCount: %u\n", t.clientCount);
    file.write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));

    if (t.probeCount > 0) {
        for (int i = 0; i < t.probeCount && i < 5; ++i) {
            if (t.probes[i][0] != '\0') {
                snprintf(buf, sizeof(buf), "Probe: %s\n", t.probes[i]);
                file.write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));
            }
        }
    }

    snprintf(buf, sizeof(buf), "Handshake: %s\n", t.hasHandshake ? "yes" : "no");
    file.write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));

    snprintf(buf, sizeof(buf), "PMKID: %s\n", t.hasPmkid ? "yes" : "no");
    file.write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));

    file.close();
    return true;
}
