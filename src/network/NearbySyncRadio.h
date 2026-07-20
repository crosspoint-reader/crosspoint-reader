#pragma once

#include <cstddef>
#include <cstdint>
#ifdef NEARBY_SYNC_TESTING
#include <array>
#include <vector>
#endif

#include "NearbySyncProtocol.h"

class NearbySyncRadio {
 public:
  using ReceiveCallback = void (*)(void* context, const NearbySync::MacAddress& sourceMac, const uint8_t* data,
                                   size_t size);

  NearbySyncRadio() = default;
  ~NearbySyncRadio();
  NearbySyncRadio(const NearbySyncRadio&) = delete;
  NearbySyncRadio& operator=(const NearbySyncRadio&) = delete;

  bool start(void* context, ReceiveCallback callback);
  void stop();
  bool addPeer(const NearbySync::MacAddress& peerMac);
  bool send(const NearbySync::MacAddress& peerMac, const uint8_t* data, size_t size) const;

  bool isStarted() const { return started_; }
  bool wasActivated() const { return activated_; }

  // Called only by the ESP-NOW driver trampoline.
  void dispatchReceived(const uint8_t* sourceMac, const uint8_t* data, size_t size) const;

#ifdef NEARBY_SYNC_TESTING
  struct TestPacket {
    NearbySync::MacAddress destination{};
    std::array<uint8_t, NearbySync::MAX_PACKET_BYTES> data{};
    size_t size = 0;
  };
  bool takeTestPacket(TestPacket& packet);
#endif

 private:
  void* callbackContext_ = nullptr;
  ReceiveCallback callback_ = nullptr;
  bool activated_ = false;
  bool initialized_ = false;
  bool callbackRegistered_ = false;
  bool started_ = false;
#ifdef NEARBY_SYNC_TESTING
  mutable std::vector<TestPacket> testPackets_;
#endif
};
