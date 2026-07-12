#include "PacketRingBuffer.h"

#include <Arduino.h>  // IRAM_ATTR, millis()
#include <algorithm>
#include <cstring>

bool IRAM_ATTR PacketRingBuffer::push(const wifi_promiscuous_pkt_t* pkt) {
    int nextWrite = (writeIdx + 1) % CAPACITY;
    if (nextWrite == readIdx) {
        // Buffer full — drop packet
        return false;
    }

    CapturedPacket& slot = buf[writeIdx];
    uint16_t copyLen = std::min((int)pkt->rx_ctrl.sig_len, 256);
    memcpy(slot.data, pkt->payload, copyLen);
    slot.len = copyLen;
    slot.rssi = pkt->rx_ctrl.rssi;
    slot.channel = pkt->rx_ctrl.channel;
    slot.timestampMs = (uint32_t)millis();

    // Publish the write — must happen after all slot writes are complete
    writeIdx = nextWrite;
    return true;
}

bool PacketRingBuffer::pop(CapturedPacket& out) {
    if (readIdx == writeIdx) {
        return false;
    }

    memcpy(&out, &buf[readIdx], sizeof(CapturedPacket));
    readIdx = (readIdx + 1) % CAPACITY;
    return true;
}

int PacketRingBuffer::available() const {
    return (writeIdx - readIdx + CAPACITY) % CAPACITY;
}

void PacketRingBuffer::clear() {
    writeIdx = 0;
    readIdx = 0;
}
