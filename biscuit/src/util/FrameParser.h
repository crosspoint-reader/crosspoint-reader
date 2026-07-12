#pragma once
#include "PacketRingBuffer.h"

namespace FrameParser {
    // Parse a captured 802.11 frame and update TargetDB
    void processPacket(const CapturedPacket& pkt);

    // Frame type checkers
    bool isBeacon(const uint8_t* data, uint16_t len);
    bool isProbeRequest(const uint8_t* data, uint16_t len);
    bool isProbeResponse(const uint8_t* data, uint16_t len);
    bool isDataFrame(const uint8_t* data, uint16_t len);
    bool isAuthFrame(const uint8_t* data, uint16_t len);
    bool isEapolFrame(const uint8_t* data, uint16_t len);

    // Extractors
    bool extractSSID(const uint8_t* data, uint16_t len, char* ssid, int maxLen);
    void extractBSSID(const uint8_t* data, uint8_t bssid[6]);
    void extractSrcMAC(const uint8_t* data, uint8_t mac[6]);
    void extractDstMAC(const uint8_t* data, uint8_t mac[6]);
    uint8_t extractChannel(const uint8_t* data, uint16_t len);
    uint8_t extractAuthType(const uint8_t* data, uint16_t len);
    bool extractPMF(const uint8_t* data, uint16_t len);
}
