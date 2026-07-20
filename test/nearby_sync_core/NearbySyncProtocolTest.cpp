#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "NearbySyncProtocol.h"

namespace {
constexpr NearbySync::MacAddress LOCAL_MAC{0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
constexpr NearbySync::MacAddress PEER_MAC{0x06, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
constexpr NearbySync::MacAddress TRANSPORT_MAC{0x0A, 0x01, 0x02, 0x03, 0x04, 0x05};

NearbySync::PacketHeader header(const NearbySync::PacketType type = NearbySync::PacketType::Offer,
                                const uint16_t sequence = 7) {
  return {NearbySync::Kind::Position, type, 0x12345678U, sequence, LOCAL_MAC};
}

std::vector<uint8_t> packetFor(const NearbySync::PacketHeader& packetHeader, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> packet(NearbySync::MAX_PACKET_BYTES);
  size_t size = 0;
  EXPECT_TRUE(
      NearbySync::encodePacket(packetHeader, payload.data(), payload.size(), packet.data(), packet.size(), size));
  packet.resize(size);
  return packet;
}
}  // namespace

TEST(NearbySyncPacket, RoundTripsHeaderAndPayload) {
  const std::vector<uint8_t> payload{1, 2, 3, 4, 5};
  const auto encoded = packetFor(header(), payload);

  NearbySync::DecodedPacket decoded;
  ASSERT_EQ(NearbySync::decodePacket(encoded.data(), encoded.size(), decoded), NearbySync::DecodeResult::Ok);
  EXPECT_EQ(decoded.header.kind, NearbySync::Kind::Position);
  EXPECT_EQ(decoded.header.type, NearbySync::PacketType::Offer);
  EXPECT_EQ(decoded.header.sessionId, 0x12345678U);
  EXPECT_EQ(decoded.header.sequence, 7);
  EXPECT_EQ(decoded.header.senderMac, LOCAL_MAC);
  ASSERT_EQ(decoded.payloadSize, payload.size());
  EXPECT_TRUE(std::equal(payload.begin(), payload.end(), decoded.payload));
}

TEST(NearbySyncPacket, RejectsCorruptionAndTrailingBytes) {
  auto encoded = packetFor(header(), {1, 2, 3});
  encoded.back() ^= 0x40;
  NearbySync::DecodedPacket decoded;
  EXPECT_EQ(NearbySync::decodePacket(encoded.data(), encoded.size(), decoded), NearbySync::DecodeResult::BadCrc);

  encoded = packetFor(header(), {1, 2, 3});
  encoded.push_back(0);
  EXPECT_EQ(NearbySync::decodePacket(encoded.data(), encoded.size(), decoded), NearbySync::DecodeResult::BadLength);
}

TEST(NearbySyncPacket, RejectsReservedIdentityAndInvalidArguments) {
  std::array<uint8_t, NearbySync::MAX_PACKET_BYTES> buffer{};
  size_t size = 99;
  auto invalid = header();
  invalid.sessionId = 0;
  EXPECT_FALSE(NearbySync::encodePacket(invalid, nullptr, 0, buffer.data(), buffer.size(), size));
  EXPECT_EQ(size, 0U);

  auto encoded = packetFor(header(), {});
  encoded[7] = 1;
  NearbySync::DecodedPacket decoded;
  EXPECT_EQ(NearbySync::decodePacket(encoded.data(), encoded.size(), decoded), NearbySync::DecodeResult::BadReserved);
}

TEST(NearbySyncSequence, HandlesReplayAndWrap) {
  EXPECT_TRUE(NearbySync::sequenceIsNewer(8, 7));
  EXPECT_FALSE(NearbySync::sequenceIsNewer(7, 7));
  EXPECT_FALSE(NearbySync::sequenceIsNewer(6, 7));
  EXPECT_TRUE(NearbySync::sequenceIsNewer(1, 65535));
  EXPECT_FALSE(NearbySync::sequenceIsNewer(0, 65535));
}

TEST(NearbySyncBinding, PinsBothMacAddressesSessionAndSequence) {
  NearbySync::PeerBinding binding;
  auto first = header(NearbySync::PacketType::Hello, 12);
  EXPECT_TRUE(binding.bind(TRANSPORT_MAC, first));
  EXPECT_TRUE(binding.isBound());
  EXPECT_EQ(binding.transportMac(), TRANSPORT_MAC);
  EXPECT_EQ(binding.deviceMac(), LOCAL_MAC);
  EXPECT_EQ(binding.sessionId(), first.sessionId);

  EXPECT_FALSE(binding.accept(TRANSPORT_MAC, first));
  auto next = first;
  next.sequence = 13;
  EXPECT_TRUE(binding.accept(TRANSPORT_MAC, next));
  next.sequence = 14;
  next.sessionId++;
  EXPECT_FALSE(binding.accept(TRANSPORT_MAC, next));
  next = first;
  next.sequence = 14;
  next.senderMac = PEER_MAC;
  EXPECT_FALSE(binding.accept(TRANSPORT_MAC, next));
  next = first;
  next.sequence = 14;
  EXPECT_FALSE(binding.accept(PEER_MAC, next));
}

TEST(NearbySyncBinding, ResetAllowsANewPeer) {
  NearbySync::PeerBinding binding;
  EXPECT_TRUE(binding.bind(TRANSPORT_MAC, header()));
  binding.reset();
  EXPECT_FALSE(binding.isBound());
  auto other = header();
  other.senderMac = PEER_MAC;
  EXPECT_TRUE(binding.bind(PEER_MAC, other));
}

TEST(NearbySyncPayload, RoundTripsBindingWithoutBroadcastingName) {
  const NearbySync::BindPayload original{0xCAFEBABEU, PEER_MAC, "CrossVi X3"};
  std::array<uint8_t, NearbySync::MAX_PAYLOAD_BYTES> bytes{};
  size_t size = 0;
  ASSERT_TRUE(NearbySync::encodeBindPayload(original, bytes.data(), bytes.size(), size));

  NearbySync::BindPayload decoded;
  ASSERT_TRUE(NearbySync::decodeBindPayload(bytes.data(), size, decoded));
  EXPECT_EQ(decoded.targetSessionId, original.targetSessionId);
  EXPECT_EQ(decoded.targetDeviceMac, original.targetDeviceMac);
  EXPECT_EQ(decoded.deviceName, original.deviceName);
  bytes[10] = 0;
  EXPECT_FALSE(NearbySync::decodeBindPayload(bytes.data(), size, decoded));
}

TEST(NearbySyncPayload, RoundTripsAcknowledgementTarget) {
  const NearbySync::AckPayload original{0x01020304U, PEER_MAC};
  std::array<uint8_t, 10> bytes{};
  size_t size = 0;
  ASSERT_TRUE(NearbySync::encodeAckPayload(original, bytes.data(), bytes.size(), size));
  EXPECT_EQ(size, 10U);
  NearbySync::AckPayload decoded;
  EXPECT_TRUE(NearbySync::decodeAckPayload(bytes.data(), size, decoded));
  EXPECT_EQ(decoded.targetSessionId, original.targetSessionId);
  EXPECT_EQ(decoded.targetDeviceMac, original.targetDeviceMac);
}

TEST(NearbySyncPosition, RoundTripsStrictOffer) {
  NearbySync::PositionOffer original;
  original.documentHash = "0123456789abcdef0123456789abcdef";
  original.percentageQ = 765432;
  original.spineIndex = 42;
  original.pageNumber = 7;
  original.totalPages = 12;
  original.paragraphIndex = 19;
  original.hasParagraphIndex = true;
  original.xpath = "/body/DocFragment[43]/body/p[19]/text().4";

  std::array<uint8_t, NearbySync::MAX_PAYLOAD_BYTES> bytes{};
  size_t size = 0;
  ASSERT_TRUE(NearbySync::encodePositionOffer(original, bytes.data(), bytes.size(), size));
  NearbySync::PositionOffer decoded;
  ASSERT_TRUE(NearbySync::decodePositionOffer(bytes.data(), size, decoded));
  EXPECT_EQ(decoded.documentHash, original.documentHash);
  EXPECT_EQ(decoded.percentageQ, original.percentageQ);
  EXPECT_EQ(decoded.spineIndex, original.spineIndex);
  EXPECT_EQ(decoded.pageNumber, original.pageNumber);
  EXPECT_EQ(decoded.totalPages, original.totalPages);
  EXPECT_EQ(decoded.paragraphIndex, original.paragraphIndex);
  EXPECT_TRUE(decoded.hasParagraphIndex);
  EXPECT_EQ(decoded.xpath, original.xpath);
}

TEST(NearbySyncPosition, RejectsUnsafeOrAmbiguousOffers) {
  NearbySync::PositionOffer offer;
  offer.documentHash = "0123456789abcdef0123456789abcdeF";
  std::array<uint8_t, NearbySync::MAX_PAYLOAD_BYTES> bytes{};
  size_t size = 0;
  EXPECT_FALSE(NearbySync::encodePositionOffer(offer, bytes.data(), bytes.size(), size));

  offer.documentHash.back() = 'f';
  offer.totalPages = 2;
  offer.pageNumber = 2;
  EXPECT_FALSE(NearbySync::encodePositionOffer(offer, bytes.data(), bytes.size(), size));

  offer.pageNumber = 1;
  ASSERT_TRUE(NearbySync::encodePositionOffer(offer, bytes.data(), bytes.size(), size));
  bytes[45] |= 0x80;
  EXPECT_FALSE(NearbySync::decodePositionOffer(bytes.data(), size, offer));
}

TEST(NearbySyncPairing, IsSymmetricAndSessionBound) {
  const uint16_t first = NearbySync::pairingCode(0x11111111U, LOCAL_MAC, 0x22222222U, PEER_MAC);
  const uint16_t reversed = NearbySync::pairingCode(0x22222222U, PEER_MAC, 0x11111111U, LOCAL_MAC);
  const uint16_t anotherSession = NearbySync::pairingCode(0x11111112U, LOCAL_MAC, 0x22222222U, PEER_MAC);
  EXPECT_EQ(first, reversed);
  EXPECT_NE(first, anotherSession);
  EXPECT_LT(first, 10000U);
}
