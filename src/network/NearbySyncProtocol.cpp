#include "NearbySyncProtocol.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace {
constexpr uint8_t MAGIC[4] = {'C', 'V', 'N', 'S'};
constexpr size_t OFFSET_VERSION = 4;
constexpr size_t OFFSET_KIND = 5;
constexpr size_t OFFSET_TYPE = 6;
constexpr size_t OFFSET_ROLE = 7;
constexpr size_t OFFSET_PAYLOAD_SIZE = 8;
constexpr size_t OFFSET_SESSION = 10;
constexpr size_t OFFSET_SEQUENCE = 14;
constexpr size_t OFFSET_MAC = 16;
constexpr size_t OFFSET_CRC = 22;
constexpr uint8_t POSITION_FORMAT_VERSION = 1;
constexpr size_t POSITION_FIXED_BYTES = 47;
constexpr uint8_t POSITION_FLAG_PARAGRAPH = 1U << 0;
constexpr uint8_t POSITION_FLAG_XPATH = 1U << 1;
constexpr uint8_t POSITION_KNOWN_FLAGS = POSITION_FLAG_PARAGRAPH | POSITION_FLAG_XPATH;

uint16_t readLe16(const uint8_t* data, const size_t offset) {
  return static_cast<uint16_t>(data[offset]) | static_cast<uint16_t>(data[offset + 1]) << 8;
}

uint32_t readLe32(const uint8_t* data, const size_t offset) {
  return static_cast<uint32_t>(data[offset]) | static_cast<uint32_t>(data[offset + 1]) << 8 |
         static_cast<uint32_t>(data[offset + 2]) << 16 | static_cast<uint32_t>(data[offset + 3]) << 24;
}

void writeLe16(uint8_t* data, const size_t offset, const uint16_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void writeLe32(uint8_t* data, const size_t offset, const uint32_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8);
  data[offset + 2] = static_cast<uint8_t>(value >> 16);
  data[offset + 3] = static_cast<uint8_t>(value >> 24);
}

bool isValidKind(const NearbySync::Kind kind) {
  return kind == NearbySync::Kind::Position || kind == NearbySync::Kind::Stats;
}

bool isValidPacketType(const NearbySync::PacketType type) {
  return type == NearbySync::PacketType::Hello || type == NearbySync::PacketType::Bind ||
         type == NearbySync::PacketType::Offer || type == NearbySync::PacketType::Ack ||
         type == NearbySync::PacketType::Complete;
}

bool isValidRole(const NearbySync::Role role) {
  return role == NearbySync::Role::Sender || role == NearbySync::Role::Receiver;
}

bool isValidDirection(const NearbySync::PacketType type, const NearbySync::Role role) {
  if (type == NearbySync::PacketType::Offer || type == NearbySync::PacketType::Complete) {
    return role == NearbySync::Role::Sender;
  }
  if (type == NearbySync::PacketType::Ack) return role == NearbySync::Role::Receiver;
  return true;
}

bool isValidUnicastMac(const NearbySync::MacAddress& mac) {
  const bool allZero = std::all_of(mac.begin(), mac.end(), [](const uint8_t value) { return value == 0; });
  return !allZero && (mac[0] & 1U) == 0;
}

uint32_t updateCrc(uint32_t crc, const uint8_t value) {
  crc ^= value;
  for (uint8_t bit = 0; bit < 8; ++bit) {
    crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
  }
  return crc;
}

uint32_t packetCrc(const uint8_t* data, const size_t size) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < size; ++i) {
    const uint8_t value = (i >= OFFSET_CRC && i < OFFSET_CRC + sizeof(uint32_t)) ? 0 : data[i];
    crc = updateCrc(crc, value);
  }
  return ~crc;
}

uint32_t bytesCrc(const uint8_t* data, const size_t size) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < size; ++i) crc = updateCrc(crc, data[i]);
  return ~crc;
}

bool isLowerHexHash(const std::string& value) {
  if (value.size() != NearbySync::DOCUMENT_HASH_HEX_BYTES) return false;
  return std::all_of(value.begin(), value.end(),
                     [](const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool isSafeText(const std::string& value, const size_t maximumBytes, const bool allowEmpty) {
  if ((!allowEmpty && value.empty()) || value.size() > maximumBytes) return false;
  return std::all_of(value.begin(), value.end(),
                     [](const unsigned char c) { return c != 0 && c != 0x7F && c >= 0x20; });
}

struct PairingParticipant {
  NearbySync::MacAddress mac{};
  uint32_t session = 0;
};

bool participantLess(const PairingParticipant& left, const PairingParticipant& right) {
  if (left.mac != right.mac) return left.mac < right.mac;
  return left.session < right.session;
}
}  // namespace

namespace NearbySync {

bool encodePacket(const PacketHeader& header, const uint8_t* payload, const size_t payloadSize, uint8_t* output,
                  const size_t capacity, size_t& outputSize) {
  outputSize = 0;
  if (!output || payloadSize > MAX_PAYLOAD_BYTES || capacity < HEADER_BYTES + payloadSize ||
      (payloadSize > 0 && !payload) || !isValidKind(header.kind) || !isValidPacketType(header.type) ||
      !isValidRole(header.role) || !isValidDirection(header.type, header.role) || header.sessionId == 0 ||
      header.sequence == 0 || !isValidUnicastMac(header.senderMac)) {
    return false;
  }

  memset(output, 0, HEADER_BYTES + payloadSize);
  memcpy(output, MAGIC, sizeof(MAGIC));
  output[OFFSET_VERSION] = PROTOCOL_VERSION;
  output[OFFSET_KIND] = static_cast<uint8_t>(header.kind);
  output[OFFSET_TYPE] = static_cast<uint8_t>(header.type);
  output[OFFSET_ROLE] = static_cast<uint8_t>(header.role);
  writeLe16(output, OFFSET_PAYLOAD_SIZE, static_cast<uint16_t>(payloadSize));
  writeLe32(output, OFFSET_SESSION, header.sessionId);
  writeLe16(output, OFFSET_SEQUENCE, header.sequence);
  memcpy(output + OFFSET_MAC, header.senderMac.data(), header.senderMac.size());
  if (payloadSize > 0) memcpy(output + HEADER_BYTES, payload, payloadSize);
  writeLe32(output, OFFSET_CRC, packetCrc(output, HEADER_BYTES + payloadSize));
  outputSize = HEADER_BYTES + payloadSize;
  return true;
}

DecodeResult decodePacket(const uint8_t* data, const size_t size, DecodedPacket& packet) {
  packet = {};
  if (!data || size < HEADER_BYTES) return DecodeResult::TooShort;
  if (size > MAX_PACKET_BYTES) return DecodeResult::TooLarge;
  if (memcmp(data, MAGIC, sizeof(MAGIC)) != 0) return DecodeResult::BadMagic;
  if (data[OFFSET_VERSION] != PROTOCOL_VERSION) return DecodeResult::BadVersion;

  const Kind kind = static_cast<Kind>(data[OFFSET_KIND]);
  if (!isValidKind(kind)) return DecodeResult::BadKind;
  const PacketType type = static_cast<PacketType>(data[OFFSET_TYPE]);
  if (!isValidPacketType(type)) return DecodeResult::BadType;
  const Role role = static_cast<Role>(data[OFFSET_ROLE]);
  if (!isValidRole(role)) return DecodeResult::BadRole;
  if (!isValidDirection(type, role)) return DecodeResult::BadDirection;

  const size_t payloadSize = readLe16(data, OFFSET_PAYLOAD_SIZE);
  if (payloadSize > MAX_PAYLOAD_BYTES || size != HEADER_BYTES + payloadSize) return DecodeResult::BadLength;

  PacketHeader header;
  header.kind = kind;
  header.type = type;
  header.role = role;
  header.sessionId = readLe32(data, OFFSET_SESSION);
  header.sequence = readLe16(data, OFFSET_SEQUENCE);
  memcpy(header.senderMac.data(), data + OFFSET_MAC, header.senderMac.size());
  if (header.sessionId == 0 || header.sequence == 0 || !isValidUnicastMac(header.senderMac)) {
    return DecodeResult::BadIdentity;
  }
  if (readLe32(data, OFFSET_CRC) != packetCrc(data, size)) return DecodeResult::BadCrc;

  packet.header = header;
  packet.payload = data + HEADER_BYTES;
  packet.payloadSize = payloadSize;
  return DecodeResult::Ok;
}

bool sequenceIsNewer(const uint16_t candidate, const uint16_t previous) {
  return candidate != 0 && candidate != previous && static_cast<int16_t>(candidate - previous) > 0;
}

bool encodeBindPayload(const BindPayload& bind, uint8_t* output, const size_t capacity, size_t& outputSize) {
  outputSize = 0;
  const size_t needed = 11 + bind.deviceName.size();
  if (!output || bind.targetSessionId == 0 || !isValidUnicastMac(bind.targetDeviceMac) ||
      !isSafeText(bind.deviceName, MAX_DEVICE_NAME_BYTES, false) || capacity < needed) {
    return false;
  }
  writeLe32(output, 0, bind.targetSessionId);
  memcpy(output + 4, bind.targetDeviceMac.data(), bind.targetDeviceMac.size());
  output[10] = static_cast<uint8_t>(bind.deviceName.size());
  memcpy(output + 11, bind.deviceName.data(), bind.deviceName.size());
  outputSize = needed;
  return true;
}

bool decodeBindPayload(const uint8_t* data, const size_t size, BindPayload& bind) {
  bind = {};
  if (!data || size < 12) return false;
  const size_t nameSize = data[10];
  if (nameSize == 0 || nameSize > MAX_DEVICE_NAME_BYTES || size != 11 + nameSize) return false;
  bind.targetSessionId = readLe32(data, 0);
  memcpy(bind.targetDeviceMac.data(), data + 4, bind.targetDeviceMac.size());
  bind.deviceName.assign(reinterpret_cast<const char*>(data + 11), nameSize);
  if (bind.targetSessionId == 0 || !isValidUnicastMac(bind.targetDeviceMac) ||
      !isSafeText(bind.deviceName, MAX_DEVICE_NAME_BYTES, false)) {
    bind = {};
    return false;
  }
  return true;
}

bool encodeAckPayload(const AckPayload& ack, uint8_t* output, const size_t capacity, size_t& outputSize) {
  outputSize = 0;
  if (!output || capacity < 10 || ack.targetSessionId == 0 || !isValidUnicastMac(ack.targetDeviceMac)) return false;
  writeLe32(output, 0, ack.targetSessionId);
  memcpy(output + 4, ack.targetDeviceMac.data(), ack.targetDeviceMac.size());
  outputSize = 10;
  return true;
}

bool decodeAckPayload(const uint8_t* data, const size_t size, AckPayload& ack) {
  ack = {};
  if (!data || size != 10) return false;
  ack.targetSessionId = readLe32(data, 0);
  memcpy(ack.targetDeviceMac.data(), data + 4, ack.targetDeviceMac.size());
  if (ack.targetSessionId == 0 || !isValidUnicastMac(ack.targetDeviceMac)) {
    ack = {};
    return false;
  }
  return true;
}

bool encodePositionOffer(const PositionOffer& offer, uint8_t* output, const size_t capacity, size_t& outputSize) {
  outputSize = 0;
  const size_t needed = POSITION_FIXED_BYTES + offer.xpath.size();
  if (!output || !isLowerHexHash(offer.documentHash) || offer.percentageQ > PERCENTAGE_SCALE || offer.totalPages == 0 ||
      offer.pageNumber >= offer.totalPages || (offer.hasParagraphIndex && offer.paragraphIndex == 0) ||
      !isSafeText(offer.xpath, MAX_XPATH_BYTES, true) || capacity < needed) {
    return false;
  }

  memset(output, 0, needed);
  output[0] = POSITION_FORMAT_VERSION;
  memcpy(output + 1, offer.documentHash.data(), offer.documentHash.size());
  writeLe32(output, 33, offer.percentageQ);
  writeLe16(output, 37, offer.spineIndex);
  writeLe16(output, 39, offer.pageNumber);
  writeLe16(output, 41, offer.totalPages);
  writeLe16(output, 43, offer.hasParagraphIndex ? offer.paragraphIndex : 0);
  output[45] =
      (offer.hasParagraphIndex ? POSITION_FLAG_PARAGRAPH : 0U) | (!offer.xpath.empty() ? POSITION_FLAG_XPATH : 0U);
  output[46] = static_cast<uint8_t>(offer.xpath.size());
  if (!offer.xpath.empty()) memcpy(output + POSITION_FIXED_BYTES, offer.xpath.data(), offer.xpath.size());
  outputSize = needed;
  return true;
}

bool decodePositionOffer(const uint8_t* data, const size_t size, PositionOffer& offer) {
  offer = {};
  if (!data || size < POSITION_FIXED_BYTES || data[0] != POSITION_FORMAT_VERSION) return false;
  const uint8_t flags = data[45];
  const size_t xpathSize = data[46];
  if ((flags & ~POSITION_KNOWN_FLAGS) != 0 || xpathSize > MAX_XPATH_BYTES || size != POSITION_FIXED_BYTES + xpathSize ||
      ((flags & POSITION_FLAG_XPATH) != 0) != (xpathSize > 0)) {
    return false;
  }

  offer.documentHash.assign(reinterpret_cast<const char*>(data + 1), DOCUMENT_HASH_HEX_BYTES);
  offer.percentageQ = readLe32(data, 33);
  offer.spineIndex = readLe16(data, 37);
  offer.pageNumber = readLe16(data, 39);
  offer.totalPages = readLe16(data, 41);
  offer.paragraphIndex = readLe16(data, 43);
  offer.hasParagraphIndex = (flags & POSITION_FLAG_PARAGRAPH) != 0;
  offer.xpath.assign(reinterpret_cast<const char*>(data + POSITION_FIXED_BYTES), xpathSize);
  if (!isLowerHexHash(offer.documentHash) || offer.percentageQ > PERCENTAGE_SCALE || offer.totalPages == 0 ||
      offer.pageNumber >= offer.totalPages || (offer.hasParagraphIndex && offer.paragraphIndex == 0) ||
      (!offer.hasParagraphIndex && offer.paragraphIndex != 0) || !isSafeText(offer.xpath, MAX_XPATH_BYTES, true)) {
    offer = {};
    return false;
  }
  return true;
}

uint16_t pairingCode(const uint32_t localSessionId, const MacAddress& localMac, const uint32_t peerSessionId,
                     const MacAddress& peerMac) {
  PairingParticipant first{localMac, localSessionId};
  PairingParticipant second{peerMac, peerSessionId};
  if (participantLess(second, first)) std::swap(first, second);

  std::array<uint8_t, 20> bytes{};
  memcpy(bytes.data(), first.mac.data(), first.mac.size());
  writeLe32(bytes.data(), 6, first.session);
  memcpy(bytes.data() + 10, second.mac.data(), second.mac.size());
  writeLe32(bytes.data(), 16, second.session);
  return static_cast<uint16_t>(bytesCrc(bytes.data(), bytes.size()) % 10000U);
}

void PeerBinding::reset() {
  bound_ = false;
  transportMac_ = {};
  deviceMac_ = {};
  role_ = Role::Sender;
  sessionId_ = 0;
  lastSequence_ = 0;
}

bool PeerBinding::bind(const MacAddress& transportMac, const PacketHeader& header) {
  if (bound_) return accept(transportMac, header);
  if (!isValidUnicastMac(transportMac) || !isValidUnicastMac(header.senderMac) || !isValidRole(header.role) ||
      header.sessionId == 0 || header.sequence == 0) {
    return false;
  }
  bound_ = true;
  transportMac_ = transportMac;
  deviceMac_ = header.senderMac;
  role_ = header.role;
  sessionId_ = header.sessionId;
  lastSequence_ = header.sequence;
  return true;
}

bool PeerBinding::accept(const MacAddress& transportMac, const PacketHeader& header) {
  if (!matches(transportMac, header) || !sequenceIsNewer(header.sequence, lastSequence_)) return false;
  lastSequence_ = header.sequence;
  return true;
}

bool PeerBinding::matches(const MacAddress& transportMac, const PacketHeader& header) const {
  return bound_ && transportMac == transportMac_ && header.senderMac == deviceMac_ && header.role == role_ &&
         header.sessionId == sessionId_;
}

}  // namespace NearbySync
