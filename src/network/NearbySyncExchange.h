#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#ifndef SIMULATOR
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

#include "NearbySyncProtocol.h"
#include "NearbySyncRadio.h"

class NearbySyncExchange {
 public:
  enum class State : uint8_t {
    Idle,
    Discovering,
    Pairing,
    WaitingForAck,
    WaitingForOffer,
    OfferReady,
    WaitingForComplete,
    Accepted,
    Error
  };
  enum class Error : uint8_t { None, RadioStart, NoPeer, PairingTimeout, TransferTimeout, QueueOverflow, Protocol };

  NearbySyncExchange();
  ~NearbySyncExchange();
  NearbySyncExchange(const NearbySyncExchange&) = delete;
  NearbySyncExchange& operator=(const NearbySyncExchange&) = delete;

  bool start(NearbySync::Kind kind, NearbySync::Role role, const NearbySync::MacAddress& localMac,
             const std::string& localDeviceName, const uint8_t* localOffer, size_t localOfferSize, uint32_t nowMs);
  void update(uint32_t nowMs);
  bool confirmPairing(uint32_t nowMs);
  bool acknowledgePeerOffer(uint32_t nowMs);
  void stop();

  State state() const { return state_; }
  Error error() const { return error_; }
  bool wasRadioActivated() const { return radio_.wasActivated(); }
  bool peerAcceptedLocalOffer() const { return peerAcceptedLocalOffer_; }
  NearbySync::Role role() const { return role_; }
  NearbySync::Role peerRole() const { return peer_.role(); }
  const std::string& peerName() const { return peerName_; }
  const NearbySync::MacAddress& peerDeviceMac() const { return peer_.deviceMac(); }
  uint16_t pairingCode() const;
  const uint8_t* peerOffer() const { return peerOffer_.data(); }
  size_t peerOfferSize() const { return peerOfferSize_; }
  bool readyToExit(uint32_t nowMs) const;

#ifdef NEARBY_SYNC_TESTING
  bool takeTestPacket(NearbySyncRadio::TestPacket& packet) { return radio_.takeTestPacket(packet); }
  void injectPacketForTest(const NearbySync::MacAddress& sourceMac, const uint8_t* data, size_t size, uint32_t nowMs) {
    handlePacket(sourceMac, data, size, nowMs);
  }
#endif

 private:
  static constexpr size_t MAX_EVENTS = 6;
  static constexpr uint32_t HELLO_INTERVAL_MS = 500;
  static constexpr uint32_t BIND_INTERVAL_MS = 700;
  static constexpr uint32_t OFFER_INTERVAL_MS = 700;
  static constexpr uint32_t ACK_INTERVAL_MS = 150;
  static constexpr uint32_t COMPLETE_INTERVAL_MS = 150;
  static constexpr uint32_t DISCOVERY_TIMEOUT_MS = 8000;
  // Pairing and local offer preview remain human-paced. Once either side has
  // explicitly confirmed and entered a transport wait, bound retries to two
  // minutes so a vanished peer cannot keep the reader awake forever.
  static constexpr uint32_t TRANSFER_TIMEOUT_MS = 120000;
  static constexpr uint32_t ACCEPT_SETTLE_MS = 900;
  static constexpr NearbySync::MacAddress BROADCAST_MAC{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

  struct RawEvent {
    NearbySync::MacAddress sourceMac{};
    std::array<uint8_t, NearbySync::MAX_PACKET_BYTES> data{};
    uint16_t size = 0;
  };

  NearbySyncRadio radio_;
  State state_ = State::Idle;
  Error error_ = Error::None;
  NearbySync::Kind kind_ = NearbySync::Kind::Position;
  NearbySync::Role role_ = NearbySync::Role::Sender;
  NearbySync::MacAddress localMac_{};
  std::string localDeviceName_;
  uint32_t localSessionId_ = 0;
  uint16_t nextSequence_ = 1;
  NearbySync::PeerBinding peer_;
  std::string peerName_;
  std::array<uint8_t, NearbySync::MAX_PAYLOAD_BYTES> localOffer_{};
  size_t localOfferSize_ = 0;
  std::array<uint8_t, NearbySync::MAX_PAYLOAD_BYTES> peerOffer_{};
  size_t peerOfferSize_ = 0;
  bool pairingConfirmed_ = false;
  bool localBindSent_ = false;
  bool peerAcceptedLocalOffer_ = false;
  uint32_t stateStartedMs_ = 0;
  uint32_t lastHelloMs_ = 0;
  uint32_t lastBindMs_ = 0;
  uint32_t lastOfferMs_ = 0;
  uint32_t lastAckMs_ = 0;
  uint32_t lastCompleteMs_ = 0;
  uint32_t acceptedMs_ = 0;

#ifndef SIMULATOR
  SemaphoreHandle_t eventMutex_ = nullptr;
#endif
  std::array<RawEvent, MAX_EVENTS> events_{};
  uint8_t eventHead_ = 0;
  uint8_t eventCount_ = 0;
  bool eventOverflow_ = false;

  static void receiveCallback(void* context, const NearbySync::MacAddress& sourceMac, const uint8_t* data, size_t size);
  void enqueue(const NearbySync::MacAddress& sourceMac, const uint8_t* data, size_t size);
  void processEvents(uint32_t nowMs);
  void handlePacket(const NearbySync::MacAddress& sourceMac, const uint8_t* data, size_t size, uint32_t nowMs);
  bool sendPacket(NearbySync::PacketType type, const NearbySync::MacAddress& destination, const uint8_t* payload,
                  size_t payloadSize);
  bool sendHello(uint32_t nowMs);
  bool sendBind(uint32_t nowMs);
  bool sendOffer(uint32_t nowMs);
  bool sendAck(uint32_t nowMs);
  bool sendComplete(uint32_t nowMs);
  void enter(State state, uint32_t nowMs);
  void fail(Error error);
};
