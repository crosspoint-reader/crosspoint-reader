#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace NearbySync {

constexpr uint8_t PROTOCOL_VERSION = 3;
constexpr size_t HEADER_BYTES = 26;
constexpr size_t MAX_PACKET_BYTES = 250;
constexpr size_t MAX_PAYLOAD_BYTES = MAX_PACKET_BYTES - HEADER_BYTES;
constexpr size_t DOCUMENT_HASH_HEX_BYTES = 32;
constexpr size_t MAX_XPATH_BYTES = 120;
constexpr size_t MAX_DEVICE_NAME_BYTES = 32;
constexpr uint32_t PERCENTAGE_SCALE = 1000000;

using MacAddress = std::array<uint8_t, 6>;

enum class Kind : uint8_t { Position = 1, Stats = 2 };
enum class PacketType : uint8_t { Hello = 1, Bind = 2, Offer = 3, Ack = 4, Complete = 5 };
enum class Role : uint8_t { Sender = 1, Receiver = 2 };

enum class DecodeResult : uint8_t {
  Ok,
  TooShort,
  TooLarge,
  BadMagic,
  BadVersion,
  BadKind,
  BadType,
  BadRole,
  BadDirection,
  BadLength,
  BadIdentity,
  BadCrc,
};

struct PacketHeader {
  Kind kind = Kind::Position;
  PacketType type = PacketType::Hello;
  Role role = Role::Sender;
  uint32_t sessionId = 0;
  uint16_t sequence = 0;
  MacAddress senderMac{};
};

struct DecodedPacket {
  PacketHeader header{};
  const uint8_t* payload = nullptr;
  size_t payloadSize = 0;
};

// Encodes one complete ESP-NOW frame. The CRC covers every header field and
// payload byte, with the four-byte CRC slot itself treated as zero.
bool encodePacket(const PacketHeader& header, const uint8_t* payload, size_t payloadSize, uint8_t* output,
                  size_t capacity, size_t& outputSize);
DecodeResult decodePacket(const uint8_t* data, size_t size, DecodedPacket& packet);

// Sequence zero is reserved. This comparison remains correct across one
// uint16_t wrap as long as fewer than 32768 packets separate the values.
bool sequenceIsNewer(uint16_t candidate, uint16_t previous);

struct BindPayload {
  uint32_t targetSessionId = 0;
  MacAddress targetDeviceMac{};
  std::string deviceName;
};

bool encodeBindPayload(const BindPayload& bind, uint8_t* output, size_t capacity, size_t& outputSize);
bool decodeBindPayload(const uint8_t* data, size_t size, BindPayload& bind);

struct AckPayload {
  uint32_t targetSessionId = 0;
  MacAddress targetDeviceMac{};
};

bool encodeAckPayload(const AckPayload& ack, uint8_t* output, size_t capacity, size_t& outputSize);
bool decodeAckPayload(const uint8_t* data, size_t size, AckPayload& ack);

struct PositionOffer {
  std::string documentHash;
  uint32_t percentageQ = 0;
  uint16_t spineIndex = 0;
  uint16_t pageNumber = 0;
  uint16_t totalPages = 1;
  uint16_t paragraphIndex = 0;
  bool hasParagraphIndex = false;
  std::string xpath;
};

bool encodePositionOffer(const PositionOffer& offer, uint8_t* output, size_t capacity, size_t& outputSize);
bool decodePositionOffer(const uint8_t* data, size_t size, PositionOffer& offer);

// Both readers derive the same four-digit code without transmitting it. Users
// can compare the code before either side sends a position or stats snapshot.
uint16_t pairingCode(uint32_t localSessionId, const MacAddress& localMac, uint32_t peerSessionId,
                     const MacAddress& peerMac);

class PeerBinding {
 public:
  void reset();
  bool bind(const MacAddress& transportMac, const PacketHeader& header);
  bool accept(const MacAddress& transportMac, const PacketHeader& header);
  bool matches(const MacAddress& transportMac, const PacketHeader& header) const;

  bool isBound() const { return bound_; }
  const MacAddress& transportMac() const { return transportMac_; }
  const MacAddress& deviceMac() const { return deviceMac_; }
  Role role() const { return role_; }
  uint32_t sessionId() const { return sessionId_; }
  uint16_t lastSequence() const { return lastSequence_; }

 private:
  bool bound_ = false;
  MacAddress transportMac_{};
  MacAddress deviceMac_{};
  Role role_ = Role::Sender;
  uint32_t sessionId_ = 0;
  uint16_t lastSequence_ = 0;
};

}  // namespace NearbySync
