#pragma once
#include <cstdint>
#include <esp_wifi_types.h>

struct CapturedPacket {
    uint8_t data[256];    // truncated frame — headers only
    uint16_t len;
    int8_t rssi;
    uint8_t channel;
    uint32_t timestampMs;
};

class PacketRingBuffer {
public:
    static constexpr int CAPACITY = 16;

    // Called from ISR context — no malloc, no logging
    bool push(const wifi_promiscuous_pkt_t* pkt);

    // Called from main loop
    bool pop(CapturedPacket& out);
    int available() const;
    void clear();

private:
    CapturedPacket buf[CAPACITY] = {};
    volatile int writeIdx = 0;
    volatile int readIdx = 0;
};
