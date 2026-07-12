#include "FrameParser.h"
#include "TargetDB.h"
#include <cstring>
#include <esp_timer.h>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Returns a pointer to the tag data for the first matching tag ID found in the
// tagged-parameter region starting at startOffset, or nullptr if not found.
// outLen is set to the tag's length field on success, 0 on failure.
static const uint8_t* findTag(const uint8_t* data, uint16_t len,
                               int startOffset, uint8_t tag, uint8_t& outLen) {
    int offset = startOffset;
    while (offset + 2 <= (int)len) {
        uint8_t tagId     = data[offset];
        uint8_t tagLength = data[offset + 1];
        if (offset + 2 + tagLength > (int)len) break;
        if (tagId == tag) {
            outLen = tagLength;
            return &data[offset + 2];
        }
        offset += 2 + tagLength;
    }
    outLen = 0;
    return nullptr;
}

// Determine the start offset of tagged parameters based on frame subtype:
//   Probe Request  (0x40): 24-byte MAC header, no fixed fields → offset 24
//   Beacon (0x80) / Probe Response (0x50): 24-byte MAC header + 12-byte fixed fields → offset 36
// Returns -1 for frame types that have no tagged parameters.
static int tagStartOffset(uint8_t fc0) {
    if (fc0 == 0x40) return 24;   // probe request
    if (fc0 == 0x80) return 36;   // beacon
    if (fc0 == 0x50) return 36;   // probe response
    return -1;
}

// ---------------------------------------------------------------------------
// Frame type checkers
// ---------------------------------------------------------------------------

bool FrameParser::isBeacon(const uint8_t* data, uint16_t len) {
    return len >= 24 && data[0] == 0x80;
}

bool FrameParser::isProbeRequest(const uint8_t* data, uint16_t len) {
    return len >= 24 && data[0] == 0x40;
}

bool FrameParser::isProbeResponse(const uint8_t* data, uint16_t len) {
    return len >= 24 && data[0] == 0x50;
}

bool FrameParser::isDataFrame(const uint8_t* data, uint16_t len) {
    return len >= 24 && (data[0] & 0x0C) == 0x08;
}

bool FrameParser::isAuthFrame(const uint8_t* data, uint16_t len) {
    return len >= 24 && data[0] == 0xB0;
}

bool FrameParser::isEapolFrame(const uint8_t* data, uint16_t len) {
    if (!isDataFrame(data, len)) return false;
    // QoS data frames have a 2-byte QoS control field after the MAC header.
    // The subtype field for QoS Data is bit 7 of FC byte 0 (0x88 etc.).
    bool qos = (data[0] & 0x80) != 0;
    int llcOffset = qos ? 26 : 24;
    if (len <= (uint16_t)(llcOffset + 8)) return false;
    // LLC/SNAP header: AA AA 03 OUI(3) EtherType(2)
    return data[llcOffset]     == 0xAA &&
           data[llcOffset + 1] == 0xAA &&
           data[llcOffset + 6] == 0x88 &&
           data[llcOffset + 7] == 0x8E;
}

// ---------------------------------------------------------------------------
// Extractors
// ---------------------------------------------------------------------------

bool FrameParser::extractSSID(const uint8_t* data, uint16_t len,
                               char* ssid, int maxLen) {
    if (len < 24 || maxLen < 1) return false;
    int startOffset = tagStartOffset(data[0]);
    if (startOffset < 0 || (int)len <= startOffset) return false;

    uint8_t tagLen = 0;
    const uint8_t* tagData = findTag(data, len, startOffset, 0, tagLen);
    if (!tagData || tagLen == 0) return false;  // not found or wildcard

    int copyLen = tagLen < (uint8_t)(maxLen - 1) ? tagLen : (maxLen - 1);
    memcpy(ssid, tagData, copyLen);
    ssid[copyLen] = '\0';
    return true;
}

void FrameParser::extractBSSID(const uint8_t* data, uint8_t bssid[6]) {
    memcpy(bssid, data + 16, 6);  // Address 3 in management frames
}

void FrameParser::extractSrcMAC(const uint8_t* data, uint8_t mac[6]) {
    memcpy(mac, data + 10, 6);    // Address 2
}

void FrameParser::extractDstMAC(const uint8_t* data, uint8_t mac[6]) {
    memcpy(mac, data + 4, 6);     // Address 1
}

uint8_t FrameParser::extractChannel(const uint8_t* data, uint16_t len) {
    if (len < 24) return 0;
    int startOffset = tagStartOffset(data[0]);
    if (startOffset < 0 || (int)len <= startOffset) return 0;

    uint8_t tagLen = 0;
    const uint8_t* tagData = findTag(data, len, startOffset, 3, tagLen);
    if (!tagData || tagLen < 1) return 0;
    return tagData[0];
}

uint8_t FrameParser::extractAuthType(const uint8_t* data, uint16_t len) {
    if (len < 24) return 0;
    int startOffset = tagStartOffset(data[0]);
    if (startOffset < 0 || (int)len <= startOffset) return 0;

    uint8_t tagLen = 0;
    const uint8_t* rsn = findTag(data, len, startOffset, 48, tagLen);
    if (!rsn || tagLen < 8) return 0;  // no RSN IE → open

    // RSN IE layout:
    //   [0-1]  version
    //   [2-5]  group cipher suite (4 bytes)
    //   [6-7]  pairwise cipher suite count
    int offset = 6;
    if (offset + 2 > tagLen) return 2;  // WPA fallback
    uint16_t pairwiseCount = (uint16_t)(rsn[offset] | (rsn[offset + 1] << 8));
    offset += 2 + pairwiseCount * 4;

    // AKM suite count
    if (offset + 2 > tagLen) return 2;
    uint16_t akmCount = (uint16_t)(rsn[offset] | (rsn[offset + 1] << 8));
    offset += 2;

    if (akmCount == 0 || offset + 4 > tagLen) return 2;

    // Inspect the first AKM suite: OUI (3 bytes) + type (1 byte)
    // OUI 00:0F:AC types: 1=802.1X, 2=PSK(WPA2), 8=SAE(WPA3)
    uint8_t akmType = rsn[offset + 3];
    switch (akmType) {
        case 8: return 4;  // WPA3-SAE
        case 2: return 3;  // WPA2-PSK
        case 1: return 3;  // WPA2-Enterprise (same bucket)
        default: return 2; // WPA
    }
}

bool FrameParser::extractPMF(const uint8_t* data, uint16_t len) {
    if (len < 24) return false;
    int startOffset = tagStartOffset(data[0]);
    if (startOffset < 0 || (int)len <= startOffset) return false;

    uint8_t tagLen = 0;
    const uint8_t* rsn = findTag(data, len, startOffset, 48, tagLen);
    if (!rsn || tagLen < 8) return false;

    // Skip version (2) + group cipher (4) + pairwise suite count+list
    int offset = 6;
    if (offset + 2 > tagLen) return false;
    uint16_t pairwiseCount = (uint16_t)(rsn[offset] | (rsn[offset + 1] << 8));
    offset += 2 + pairwiseCount * 4;

    // Skip AKM suite count+list
    if (offset + 2 > tagLen) return false;
    uint16_t akmCount = (uint16_t)(rsn[offset] | (rsn[offset + 1] << 8));
    offset += 2 + akmCount * 4;

    // RSN Capabilities (2 bytes)
    if (offset + 2 > tagLen) return false;
    uint16_t rsnCaps = (uint16_t)(rsn[offset] | (rsn[offset + 1] << 8));
    // Bit 6 = MFPR (Management Frame Protection Required)
    return (rsnCaps & (1u << 6)) != 0;
}

// ---------------------------------------------------------------------------
// processPacket
// ---------------------------------------------------------------------------

void FrameParser::processPacket(const CapturedPacket& pkt) {
    const uint8_t* data = pkt.data;
    uint16_t len = pkt.len;

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000ULL);

    if (isBeacon(data, len) || isProbeResponse(data, len)) {
        Target t;
        t.type     = TargetType::AP;
        extractBSSID(data, t.mac);
        extractSSID(data, len, t.ssid, sizeof(t.ssid));
        t.channel  = extractChannel(data, len);
        t.authType = extractAuthType(data, len);
        t.pmf      = extractPMF(data, len);
        t.rssi     = pkt.rssi;
        t.lastSeen  = now;
        t.firstSeen = now;
        TARGETS.addOrUpdate(t);
    }
    else if (isProbeRequest(data, len)) {
        Target t;
        t.type = TargetType::STA;
        extractSrcMAC(data, t.mac);
        t.rssi      = pkt.rssi;
        t.lastSeen  = now;
        t.firstSeen = now;

        char probedSsid[33] = {};
        if (extractSSID(data, len, probedSsid, sizeof(probedSsid)) &&
            probedSsid[0] != '\0') {
            strncpy(t.probes[0], probedSsid, 32);
            t.probeCount = 1;
        }
        TARGETS.addOrUpdate(t);
    }
    else if (isDataFrame(data, len)) {
        // ToDS is bit 0 of FC byte 1; FromDS is bit 1 of FC byte 1.
        uint8_t toDS   = data[1] & 0x01;
        uint8_t fromDS = (data[1] >> 1) & 0x01;

        if (toDS && !fromDS) {
            // Client → AP: Addr1=BSSID(AP), Addr2=Source(client), Addr3=Dest
            Target client;
            client.type = TargetType::STA;
            memcpy(client.mac,   data + 10, 6);  // Address 2 = client
            memcpy(client.bssid, data + 4,  6);  // Address 1 = AP BSSID
            client.rssi      = pkt.rssi;
            client.lastSeen  = now;
            client.firstSeen = now;
            TARGETS.addOrUpdate(client);
        }
        else if (!toDS && fromDS) {
            // AP → Client: Addr1=Dest(client), Addr2=BSSID(AP), Addr3=Source
            Target client;
            client.type = TargetType::STA;
            memcpy(client.mac,   data + 4,  6);  // Address 1 = client
            memcpy(client.bssid, data + 10, 6);  // Address 2 = AP BSSID
            client.rssi      = pkt.rssi;
            client.lastSeen  = now;
            client.firstSeen = now;
            TARGETS.addOrUpdate(client);
        }
    }
    // EAPOL frames are handled separately by FireActivity's capture callback.
}
