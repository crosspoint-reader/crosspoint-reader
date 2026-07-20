#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "NearbySyncExchange.h"

namespace {
constexpr NearbySync::MacAddress FIRST_MAC{0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
constexpr NearbySync::MacAddress SECOND_MAC{0x06, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
constexpr std::array<uint8_t, 3> FIRST_OFFER{1, 2, 3};
constexpr std::array<uint8_t, 3> SECOND_OFFER{4, 5, 6};

NearbySyncRadio::TestPacket takePacket(NearbySyncExchange& exchange, const NearbySync::PacketType type) {
  NearbySyncRadio::TestPacket packet;
  while (exchange.takeTestPacket(packet)) {
    NearbySync::DecodedPacket decoded;
    if (NearbySync::decodePacket(packet.data.data(), packet.size, decoded) == NearbySync::DecodeResult::Ok &&
        decoded.header.type == type) {
      return packet;
    }
  }
  ADD_FAILURE() << "Expected packet type " << static_cast<unsigned>(type);
  return {};
}

void deliver(NearbySyncExchange& receiver, const NearbySync::MacAddress& sender,
             const NearbySyncRadio::TestPacket& packet, const uint32_t nowMs) {
  ASSERT_GT(packet.size, 0U);
  receiver.injectPacketForTest(sender, packet.data.data(), packet.size, nowMs);
}

void pairReaders(NearbySyncExchange& first, NearbySyncExchange& second) {
  ASSERT_TRUE(first.start(NearbySync::Kind::Position, FIRST_MAC, "First", FIRST_OFFER.data(), FIRST_OFFER.size(), 0));
  ASSERT_TRUE(
      second.start(NearbySync::Kind::Position, SECOND_MAC, "Second", SECOND_OFFER.data(), SECOND_OFFER.size(), 0));

  const auto firstHello = takePacket(first, NearbySync::PacketType::Hello);
  const auto secondHello = takePacket(second, NearbySync::PacketType::Hello);
  deliver(first, SECOND_MAC, secondHello, 1);
  deliver(second, FIRST_MAC, firstHello, 1);

  const auto firstBind = takePacket(first, NearbySync::PacketType::Bind);
  const auto secondBind = takePacket(second, NearbySync::PacketType::Bind);
  deliver(first, SECOND_MAC, secondBind, 2);
  deliver(second, FIRST_MAC, firstBind, 2);

  ASSERT_EQ(first.state(), NearbySyncExchange::State::Pairing);
  ASSERT_EQ(second.state(), NearbySyncExchange::State::Pairing);
  ASSERT_EQ(first.pairingCode(), second.pairingCode());
}
}  // namespace

TEST(NearbySyncExchange, HumanConfirmationScreensDoNotExpireOnATransportTimer) {
  NearbySyncExchange first;
  NearbySyncExchange second;
  pairReaders(first, second);

  first.update(5U * 60U * 1000U);
  EXPECT_EQ(first.state(), NearbySyncExchange::State::Pairing);

  ASSERT_TRUE(first.confirmShare(5U * 60U * 1000U + 1U));
  takePacket(first, NearbySync::PacketType::Offer);
  first.update(10U * 60U * 1000U);
  EXPECT_EQ(first.state(), NearbySyncExchange::State::WaitingForOffer);
  EXPECT_GT(takePacket(first, NearbySync::PacketType::Offer).size, 0U);
}

TEST(NearbySyncExchange, AcceptedSideRetransmitsItsOfferUntilReciprocalAck) {
  NearbySyncExchange first;
  NearbySyncExchange second;
  pairReaders(first, second);

  ASSERT_TRUE(first.confirmShare(10));
  ASSERT_TRUE(second.confirmShare(10));
  takePacket(first, NearbySync::PacketType::Offer);  // Simulate loss only in this direction.
  const auto secondOffer = takePacket(second, NearbySync::PacketType::Offer);
  deliver(first, SECOND_MAC, secondOffer, 11);
  ASSERT_EQ(first.state(), NearbySyncExchange::State::OfferReady);
  ASSERT_EQ(second.state(), NearbySyncExchange::State::WaitingForOffer);

  ASSERT_TRUE(first.acknowledgePeerOffer(20));
  const auto firstAck = takePacket(first, NearbySync::PacketType::Ack);
  deliver(second, FIRST_MAC, firstAck, 21);
  ASSERT_TRUE(second.peerAcceptedLocalOffer());
  ASSERT_EQ(second.state(), NearbySyncExchange::State::WaitingForOffer);

  first.update(800);
  const auto retriedFirstOffer = takePacket(first, NearbySync::PacketType::Offer);
  deliver(second, FIRST_MAC, retriedFirstOffer, 801);
  ASSERT_EQ(first.state(), NearbySyncExchange::State::Accepted);
  ASSERT_EQ(second.state(), NearbySyncExchange::State::OfferReady);

  ASSERT_TRUE(second.acknowledgePeerOffer(810));
  const auto secondAck = takePacket(second, NearbySync::PacketType::Ack);
  deliver(first, SECOND_MAC, secondAck, 811);
  EXPECT_TRUE(first.peerAcceptedLocalOffer());
  EXPECT_FALSE(first.readyToExit(919));
  EXPECT_TRUE(first.readyToExit(920));
  EXPECT_FALSE(second.readyToExit(1709));
  EXPECT_TRUE(second.readyToExit(1710));
}
