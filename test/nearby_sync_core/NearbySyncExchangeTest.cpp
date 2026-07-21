#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "NearbySyncExchange.h"

namespace {
constexpr NearbySync::MacAddress FIRST_MAC{0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
constexpr NearbySync::MacAddress SECOND_MAC{0x06, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
constexpr std::array<uint8_t, 3> FIRST_OFFER{1, 2, 3};

bool tryTakePacket(NearbySyncExchange& exchange, const NearbySync::PacketType type,
                   NearbySyncRadio::TestPacket& result) {
  NearbySyncRadio::TestPacket packet;
  while (exchange.takeTestPacket(packet)) {
    NearbySync::DecodedPacket decoded;
    if (NearbySync::decodePacket(packet.data.data(), packet.size, decoded) == NearbySync::DecodeResult::Ok &&
        decoded.header.type == type) {
      result = packet;
      return true;
    }
  }
  return false;
}

NearbySyncRadio::TestPacket takePacket(NearbySyncExchange& exchange, const NearbySync::PacketType type) {
  NearbySyncRadio::TestPacket packet;
  EXPECT_TRUE(tryTakePacket(exchange, type, packet)) << "Expected packet type " << static_cast<unsigned>(type);
  return packet;
}

NearbySync::DecodedPacket decode(const NearbySyncRadio::TestPacket& packet) {
  NearbySync::DecodedPacket decoded;
  EXPECT_EQ(NearbySync::decodePacket(packet.data.data(), packet.size, decoded), NearbySync::DecodeResult::Ok);
  return decoded;
}

NearbySyncRadio::TestPacket makePacket(const NearbySync::PacketHeader& header, const uint8_t* payload,
                                       const size_t payloadSize) {
  NearbySyncRadio::TestPacket packet;
  EXPECT_TRUE(
      NearbySync::encodePacket(header, payload, payloadSize, packet.data.data(), packet.data.size(), packet.size));
  return packet;
}

void deliver(NearbySyncExchange& receiver, const NearbySync::MacAddress& sender,
             const NearbySyncRadio::TestPacket& packet, const uint32_t nowMs) {
  ASSERT_GT(packet.size, 0U);
  receiver.injectPacketForTest(sender, packet.data.data(), packet.size, nowMs);
}

void pairReaders(NearbySyncExchange& sender, NearbySyncExchange& receiver) {
  ASSERT_TRUE(sender.start(NearbySync::Kind::Position, NearbySync::Role::Sender, FIRST_MAC, "Sender",
                           FIRST_OFFER.data(), FIRST_OFFER.size(), 0));
  ASSERT_TRUE(
      receiver.start(NearbySync::Kind::Position, NearbySync::Role::Receiver, SECOND_MAC, "Receiver", nullptr, 0, 0));

  const auto senderHello = takePacket(sender, NearbySync::PacketType::Hello);
  const auto receiverHello = takePacket(receiver, NearbySync::PacketType::Hello);
  deliver(sender, SECOND_MAC, receiverHello, 1);
  deliver(receiver, FIRST_MAC, senderHello, 1);

  ASSERT_EQ(sender.state(), NearbySyncExchange::State::Discovering);
  ASSERT_EQ(receiver.state(), NearbySyncExchange::State::Discovering);
  const auto senderBind = takePacket(sender, NearbySync::PacketType::Bind);
  const auto receiverBind = takePacket(receiver, NearbySync::PacketType::Bind);
  deliver(sender, SECOND_MAC, receiverBind, 2);
  deliver(receiver, FIRST_MAC, senderBind, 2);

  ASSERT_EQ(sender.state(), NearbySyncExchange::State::Pairing);
  ASSERT_EQ(receiver.state(), NearbySyncExchange::State::Pairing);
  ASSERT_EQ(sender.pairingCode(), receiver.pairingCode());
  ASSERT_EQ(sender.peerRole(), NearbySync::Role::Receiver);
  ASSERT_EQ(receiver.peerRole(), NearbySync::Role::Sender);
}

void confirmAndDeliverOffer(NearbySyncExchange& sender, NearbySyncExchange& receiver) {
  ASSERT_TRUE(receiver.confirmPairing(10));
  ASSERT_TRUE(sender.confirmPairing(11));
  const auto offer = takePacket(sender, NearbySync::PacketType::Offer);
  deliver(receiver, FIRST_MAC, offer, 12);
  ASSERT_EQ(sender.state(), NearbySyncExchange::State::WaitingForAck);
  ASSERT_EQ(receiver.state(), NearbySyncExchange::State::OfferReady);
}
}  // namespace

TEST(NearbySyncExchange, EnforcesRoleSpecificStartContracts) {
  NearbySyncExchange exchange;
  EXPECT_FALSE(
      exchange.start(NearbySync::Kind::Position, NearbySync::Role::Sender, FIRST_MAC, "Sender", nullptr, 0, 0));
  EXPECT_FALSE(exchange.start(NearbySync::Kind::Position, NearbySync::Role::Receiver, FIRST_MAC, "Receiver",
                              FIRST_OFFER.data(), FIRST_OFFER.size(), 0));
  EXPECT_FALSE(exchange.start(NearbySync::Kind::Position, static_cast<NearbySync::Role>(9), FIRST_MAC, "Invalid",
                              nullptr, 0, 0));
  EXPECT_TRUE(
      exchange.start(NearbySync::Kind::Position, NearbySync::Role::Receiver, FIRST_MAC, "Receiver", nullptr, 0, 0));
  EXPECT_EQ(exchange.role(), NearbySync::Role::Receiver);
}

TEST(NearbySyncExchange, RequiresAReciprocalBindTargetingTheCurrentSession) {
  NearbySyncExchange receiver;
  ASSERT_TRUE(
      receiver.start(NearbySync::Kind::Position, NearbySync::Role::Receiver, SECOND_MAC, "Receiver", nullptr, 0, 0));
  const auto receiverHello = takePacket(receiver, NearbySync::PacketType::Hello);
  const auto receiverHeader = decode(receiverHello).header;

  const NearbySync::PacketHeader peerHello{
      NearbySync::Kind::Position, NearbySync::PacketType::Hello, NearbySync::Role::Sender, 0x10203040U, 1, FIRST_MAC};
  deliver(receiver, FIRST_MAC, makePacket(peerHello, nullptr, 0), 1);
  EXPECT_EQ(receiver.state(), NearbySyncExchange::State::Discovering);
  EXPECT_FALSE(receiver.confirmPairing(2));
  takePacket(receiver, NearbySync::PacketType::Bind);

  NearbySync::BindPayload bind{receiverHeader.sessionId + 1U, SECOND_MAC, "Sender"};
  std::array<uint8_t, NearbySync::MAX_PAYLOAD_BYTES> payload{};
  size_t payloadSize = 0;
  ASSERT_TRUE(NearbySync::encodeBindPayload(bind, payload.data(), payload.size(), payloadSize));
  auto bindHeader = peerHello;
  bindHeader.type = NearbySync::PacketType::Bind;
  bindHeader.sequence = 2;
  deliver(receiver, FIRST_MAC, makePacket(bindHeader, payload.data(), payloadSize), 3);
  EXPECT_EQ(receiver.state(), NearbySyncExchange::State::Discovering);

  bind.targetSessionId = receiverHeader.sessionId;
  ASSERT_TRUE(NearbySync::encodeBindPayload(bind, payload.data(), payload.size(), payloadSize));
  deliver(receiver, FIRST_MAC, makePacket(bindHeader, payload.data(), payloadSize), 4);
  EXPECT_EQ(receiver.state(), NearbySyncExchange::State::Pairing);
  EXPECT_EQ(receiver.peerName(), "Sender");
}

TEST(NearbySyncExchange, SameRolesNeverPair) {
  NearbySyncExchange first;
  NearbySyncExchange second;
  ASSERT_TRUE(first.start(NearbySync::Kind::Position, NearbySync::Role::Sender, FIRST_MAC, "First", FIRST_OFFER.data(),
                          FIRST_OFFER.size(), 0));
  ASSERT_TRUE(second.start(NearbySync::Kind::Position, NearbySync::Role::Sender, SECOND_MAC, "Second",
                           FIRST_OFFER.data(), FIRST_OFFER.size(), 0));
  const auto firstHello = takePacket(first, NearbySync::PacketType::Hello);
  const auto secondHello = takePacket(second, NearbySync::PacketType::Hello);
  deliver(first, SECOND_MAC, secondHello, 1);
  deliver(second, FIRST_MAC, firstHello, 1);
  EXPECT_EQ(first.state(), NearbySyncExchange::State::Discovering);
  EXPECT_EQ(second.state(), NearbySyncExchange::State::Discovering);
  NearbySyncRadio::TestPacket unused;
  EXPECT_FALSE(tryTakePacket(first, NearbySync::PacketType::Bind, unused));
  EXPECT_FALSE(tryTakePacket(second, NearbySync::PacketType::Bind, unused));
  first.update(8000);
  second.update(8000);
  EXPECT_EQ(first.error(), NearbySyncExchange::Error::NoPeer);
  EXPECT_EQ(second.error(), NearbySyncExchange::Error::NoPeer);
}

TEST(NearbySyncExchange, HumanConfirmationScreensDoNotExpireOnATransportTimer) {
  NearbySyncExchange sender;
  NearbySyncExchange receiver;
  pairReaders(sender, receiver);

  sender.update(5U * 60U * 1000U);
  EXPECT_EQ(sender.state(), NearbySyncExchange::State::Pairing);

  ASSERT_TRUE(receiver.confirmPairing(5U * 60U * 1000U + 1U));
  ASSERT_TRUE(sender.confirmPairing(5U * 60U * 1000U + 2U));
  const auto offer = takePacket(sender, NearbySync::PacketType::Offer);
  deliver(receiver, FIRST_MAC, offer, 5U * 60U * 1000U + 3U);
  ASSERT_EQ(receiver.state(), NearbySyncExchange::State::OfferReady);
  receiver.update(10U * 60U * 1000U);
  EXPECT_EQ(receiver.state(), NearbySyncExchange::State::OfferReady);
  EXPECT_EQ(receiver.error(), NearbySyncExchange::Error::None);
}

TEST(NearbySyncExchange, SenderTimesOutWhenEveryAckIsLost) {
  NearbySyncExchange sender;
  NearbySyncExchange receiver;
  pairReaders(sender, receiver);
  confirmAndDeliverOffer(sender, receiver);

  ASSERT_TRUE(receiver.acknowledgePeerOffer(20));
  takePacket(receiver, NearbySync::PacketType::Ack);  // Drop the initial ACK.
  receiver.update(170);
  takePacket(receiver, NearbySync::PacketType::Ack);  // Drop a retry too.

  sender.update(120011);
  EXPECT_EQ(sender.state(), NearbySyncExchange::State::Error);
  EXPECT_EQ(sender.error(), NearbySyncExchange::Error::TransferTimeout);
}

TEST(NearbySyncExchange, ReceiverTimesOutWhenEveryOfferIsLost) {
  NearbySyncExchange sender;
  NearbySyncExchange receiver;
  pairReaders(sender, receiver);
  ASSERT_TRUE(receiver.confirmPairing(10));
  ASSERT_TRUE(sender.confirmPairing(11));
  takePacket(sender, NearbySync::PacketType::Offer);  // Drop the initial offer.
  sender.update(711);
  takePacket(sender, NearbySync::PacketType::Offer);  // Drop a retry too.

  receiver.update(120010);
  EXPECT_EQ(receiver.state(), NearbySyncExchange::State::Error);
  EXPECT_EQ(receiver.error(), NearbySyncExchange::Error::TransferTimeout);
}

TEST(NearbySyncExchange, ReceiverTimesOutWhenEveryCompletionIsLost) {
  NearbySyncExchange sender;
  NearbySyncExchange receiver;
  pairReaders(sender, receiver);
  confirmAndDeliverOffer(sender, receiver);
  ASSERT_TRUE(receiver.acknowledgePeerOffer(20));
  const auto ack = takePacket(receiver, NearbySync::PacketType::Ack);
  deliver(sender, SECOND_MAC, ack, 21);
  takePacket(sender, NearbySync::PacketType::Complete);  // Drop the initial completion.
  sender.update(171);
  takePacket(sender, NearbySync::PacketType::Complete);  // Drop a retry too.

  receiver.update(120020);
  EXPECT_EQ(receiver.state(), NearbySyncExchange::State::Error);
  EXPECT_EQ(receiver.error(), NearbySyncExchange::Error::TransferTimeout);
}

TEST(NearbySyncExchange, SimultaneousConfirmationTransfersOnlySenderOffer) {
  NearbySyncExchange sender;
  NearbySyncExchange receiver;
  pairReaders(sender, receiver);

  ASSERT_TRUE(sender.confirmPairing(10));
  ASSERT_TRUE(receiver.confirmPairing(10));
  EXPECT_EQ(sender.state(), NearbySyncExchange::State::WaitingForAck);
  EXPECT_EQ(receiver.state(), NearbySyncExchange::State::WaitingForOffer);
  NearbySyncRadio::TestPacket unexpected;
  EXPECT_FALSE(tryTakePacket(receiver, NearbySync::PacketType::Offer, unexpected));

  const auto offer = takePacket(sender, NearbySync::PacketType::Offer);
  deliver(receiver, FIRST_MAC, offer, 11);
  ASSERT_EQ(receiver.state(), NearbySyncExchange::State::OfferReady);
  ASSERT_EQ(receiver.peerOfferSize(), FIRST_OFFER.size());
  EXPECT_TRUE(std::equal(FIRST_OFFER.begin(), FIRST_OFFER.end(), receiver.peerOffer()));
  EXPECT_EQ(sender.peerOfferSize(), 0U);

  ASSERT_TRUE(receiver.acknowledgePeerOffer(20));
  EXPECT_EQ(receiver.state(), NearbySyncExchange::State::WaitingForComplete);
  const auto ack = takePacket(receiver, NearbySync::PacketType::Ack);
  deliver(sender, SECOND_MAC, ack, 21);
  EXPECT_EQ(sender.state(), NearbySyncExchange::State::Accepted);
  EXPECT_EQ(receiver.state(), NearbySyncExchange::State::WaitingForComplete);
  const auto complete = takePacket(sender, NearbySync::PacketType::Complete);
  deliver(receiver, FIRST_MAC, complete, 22);
  EXPECT_EQ(receiver.state(), NearbySyncExchange::State::Accepted);
  EXPECT_TRUE(sender.peerAcceptedLocalOffer());
  EXPECT_FALSE(receiver.readyToExit(921));
  EXPECT_TRUE(receiver.readyToExit(922));
  EXPECT_FALSE(sender.readyToExit(920));
  EXPECT_TRUE(sender.readyToExit(921));
}

TEST(NearbySyncExchange, SenderRetriesAnOfferConfirmedBeforeReceiver) {
  NearbySyncExchange sender;
  NearbySyncExchange receiver;
  pairReaders(sender, receiver);

  ASSERT_TRUE(sender.confirmPairing(10));
  takePacket(sender, NearbySync::PacketType::Offer);  // Lost before the receiver confirms.
  ASSERT_TRUE(receiver.confirmPairing(20));
  sender.update(711);
  const auto retry = takePacket(sender, NearbySync::PacketType::Offer);
  deliver(receiver, FIRST_MAC, retry, 712);
  EXPECT_EQ(receiver.state(), NearbySyncExchange::State::OfferReady);
}

TEST(NearbySyncExchange, ReceiverRetriesALostAckDuringSettle) {
  NearbySyncExchange sender;
  NearbySyncExchange receiver;
  pairReaders(sender, receiver);
  confirmAndDeliverOffer(sender, receiver);

  ASSERT_TRUE(receiver.acknowledgePeerOffer(20));
  takePacket(receiver, NearbySync::PacketType::Ack);  // Lost.
  EXPECT_EQ(receiver.state(), NearbySyncExchange::State::WaitingForComplete);
  EXPECT_EQ(sender.state(), NearbySyncExchange::State::WaitingForAck);
  receiver.update(170);
  const auto retry = takePacket(receiver, NearbySync::PacketType::Ack);
  deliver(sender, SECOND_MAC, retry, 171);
  EXPECT_EQ(sender.state(), NearbySyncExchange::State::Accepted);
  EXPECT_EQ(receiver.state(), NearbySyncExchange::State::WaitingForComplete);

  const auto complete = takePacket(sender, NearbySync::PacketType::Complete);
  deliver(receiver, FIRST_MAC, complete, 172);
  EXPECT_EQ(receiver.state(), NearbySyncExchange::State::Accepted);
}

TEST(NearbySyncExchange, ReceiverDoesNotFalseCompleteWhenAllAcksAreLostPastTheOldSettleWindow) {
  NearbySyncExchange sender;
  NearbySyncExchange receiver;
  pairReaders(sender, receiver);
  confirmAndDeliverOffer(sender, receiver);

  ASSERT_TRUE(receiver.acknowledgePeerOffer(20));
  takePacket(receiver, NearbySync::PacketType::Ack);  // Drop the first ACK.
  for (uint32_t now = 170; now <= 1070; now += 150) {
    receiver.update(now);
    takePacket(receiver, NearbySync::PacketType::Ack);  // Drop every retry beyond 900 ms.
  }
  EXPECT_EQ(receiver.state(), NearbySyncExchange::State::WaitingForComplete);
  EXPECT_FALSE(receiver.readyToExit(5000));
  EXPECT_EQ(sender.state(), NearbySyncExchange::State::WaitingForAck);

  receiver.update(1220);
  const auto recoveredAck = takePacket(receiver, NearbySync::PacketType::Ack);
  deliver(sender, SECOND_MAC, recoveredAck, 1221);
  const auto complete = takePacket(sender, NearbySync::PacketType::Complete);
  deliver(receiver, FIRST_MAC, complete, 1222);
  EXPECT_EQ(receiver.state(), NearbySyncExchange::State::Accepted);
}

TEST(NearbySyncExchange, SenderRetriesADroppedCompletionPacket) {
  NearbySyncExchange sender;
  NearbySyncExchange receiver;
  pairReaders(sender, receiver);
  confirmAndDeliverOffer(sender, receiver);

  ASSERT_TRUE(receiver.acknowledgePeerOffer(20));
  const auto ack = takePacket(receiver, NearbySync::PacketType::Ack);
  deliver(sender, SECOND_MAC, ack, 21);
  takePacket(sender, NearbySync::PacketType::Complete);  // Lost.
  EXPECT_EQ(receiver.state(), NearbySyncExchange::State::WaitingForComplete);

  sender.update(171);
  const auto retry = takePacket(sender, NearbySync::PacketType::Complete);
  deliver(receiver, FIRST_MAC, retry, 172);
  EXPECT_EQ(receiver.state(), NearbySyncExchange::State::Accepted);

  sender.update(321);
  const auto duplicate = takePacket(sender, NearbySync::PacketType::Complete);
  deliver(receiver, FIRST_MAC, duplicate, 322);
  EXPECT_EQ(receiver.state(), NearbySyncExchange::State::Accepted);
  EXPECT_EQ(receiver.error(), NearbySyncExchange::Error::None);
}

TEST(NearbySyncExchange, CallerCanCancelWhileWaitingForCompletion) {
  NearbySyncExchange sender;
  NearbySyncExchange receiver;
  pairReaders(sender, receiver);
  confirmAndDeliverOffer(sender, receiver);

  ASSERT_TRUE(receiver.acknowledgePeerOffer(20));
  takePacket(receiver, NearbySync::PacketType::Ack);
  ASSERT_EQ(receiver.state(), NearbySyncExchange::State::WaitingForComplete);

  // Nearby activities map Back to finish(); onExit() calls stop(). The
  // transport must therefore leave the non-expiring wait immediately.
  receiver.stop();
  EXPECT_EQ(receiver.state(), NearbySyncExchange::State::Idle);
  EXPECT_FALSE(receiver.readyToExit(5000));
}

TEST(NearbySyncExchange, OldCompletionCannotFinishARestartedReceiverSession) {
  NearbySyncExchange sender;
  NearbySyncExchange receiver;
  pairReaders(sender, receiver);
  confirmAndDeliverOffer(sender, receiver);
  ASSERT_TRUE(receiver.acknowledgePeerOffer(20));
  const auto firstAck = takePacket(receiver, NearbySync::PacketType::Ack);
  deliver(sender, SECOND_MAC, firstAck, 21);
  const auto oldComplete = takePacket(sender, NearbySync::PacketType::Complete);

  pairReaders(sender, receiver);
  confirmAndDeliverOffer(sender, receiver);
  ASSERT_TRUE(receiver.acknowledgePeerOffer(30));
  takePacket(receiver, NearbySync::PacketType::Ack);
  deliver(receiver, FIRST_MAC, oldComplete, 31);
  EXPECT_EQ(receiver.state(), NearbySyncExchange::State::WaitingForComplete);
  EXPECT_FALSE(receiver.readyToExit(5000));
}

TEST(NearbySyncExchange, IgnoresDuplicateAndOutOfOrderOffers) {
  NearbySyncExchange sender;
  NearbySyncExchange receiver;
  pairReaders(sender, receiver);
  ASSERT_TRUE(receiver.confirmPairing(10));
  ASSERT_TRUE(sender.confirmPairing(11));
  const auto older = takePacket(sender, NearbySync::PacketType::Offer);
  sender.update(711);
  const auto newer = takePacket(sender, NearbySync::PacketType::Offer);

  deliver(receiver, FIRST_MAC, newer, 712);
  ASSERT_EQ(receiver.state(), NearbySyncExchange::State::OfferReady);
  deliver(receiver, FIRST_MAC, older, 713);
  deliver(receiver, FIRST_MAC, newer, 714);
  EXPECT_EQ(receiver.state(), NearbySyncExchange::State::OfferReady);
  EXPECT_EQ(receiver.error(), NearbySyncExchange::Error::None);
}

TEST(NearbySyncExchange, IgnoresRoleChangesWithoutPoisoningThePinnedPeer) {
  NearbySyncExchange sender;
  NearbySyncExchange receiver;
  pairReaders(sender, receiver);
  ASSERT_TRUE(receiver.confirmPairing(10));
  ASSERT_TRUE(sender.confirmPairing(11));
  const auto offer = takePacket(sender, NearbySync::PacketType::Offer);
  auto changedHeader = decode(offer).header;
  changedHeader.type = NearbySync::PacketType::Hello;
  changedHeader.role = NearbySync::Role::Receiver;
  ++changedHeader.sequence;
  deliver(receiver, FIRST_MAC, makePacket(changedHeader, nullptr, 0), 12);
  EXPECT_EQ(receiver.state(), NearbySyncExchange::State::WaitingForOffer);

  deliver(receiver, FIRST_MAC, offer, 13);
  EXPECT_EQ(receiver.state(), NearbySyncExchange::State::OfferReady);
}

TEST(NearbySyncExchange, OldHelloAndOfferCannotBypassCurrentTargetedBind) {
  NearbySyncExchange receiver;
  ASSERT_TRUE(
      receiver.start(NearbySync::Kind::Position, NearbySync::Role::Receiver, SECOND_MAC, "Receiver", nullptr, 0, 0));
  takePacket(receiver, NearbySync::PacketType::Hello);
  NearbySync::PacketHeader oldHeader{
      NearbySync::Kind::Position, NearbySync::PacketType::Hello, NearbySync::Role::Sender, 0x55667788U, 1, FIRST_MAC};
  deliver(receiver, FIRST_MAC, makePacket(oldHeader, nullptr, 0), 1);
  ASSERT_EQ(receiver.state(), NearbySyncExchange::State::Discovering);
  oldHeader.type = NearbySync::PacketType::Offer;
  oldHeader.sequence = 2;
  deliver(receiver, FIRST_MAC, makePacket(oldHeader, FIRST_OFFER.data(), FIRST_OFFER.size()), 2);
  EXPECT_EQ(receiver.state(), NearbySyncExchange::State::Discovering);
  EXPECT_EQ(receiver.peerOfferSize(), 0U);
  EXPECT_FALSE(receiver.confirmPairing(3));
  receiver.update(8000);
  EXPECT_EQ(receiver.error(), NearbySyncExchange::Error::PairingTimeout);
}

TEST(NearbySyncExchange, OldAckCannotAcceptARestartedSenderSession) {
  NearbySyncExchange sender;
  NearbySyncExchange receiver;
  pairReaders(sender, receiver);
  confirmAndDeliverOffer(sender, receiver);
  ASSERT_TRUE(receiver.acknowledgePeerOffer(20));
  const auto oldAck = takePacket(receiver, NearbySync::PacketType::Ack);

  pairReaders(sender, receiver);
  confirmAndDeliverOffer(sender, receiver);
  deliver(sender, SECOND_MAC, oldAck, 30);
  EXPECT_EQ(sender.state(), NearbySyncExchange::State::WaitingForAck);
  EXPECT_FALSE(sender.peerAcceptedLocalOffer());
}

TEST(NearbySyncExchange, RejectsAnOfferThatMutatesWithinTheSession) {
  NearbySyncExchange sender;
  NearbySyncExchange receiver;
  pairReaders(sender, receiver);
  confirmAndDeliverOffer(sender, receiver);

  sender.update(711);
  auto changedHeader = decode(takePacket(sender, NearbySync::PacketType::Offer)).header;
  ++changedHeader.sequence;
  const std::array<uint8_t, 3> changedOffer{1, 2, 4};
  deliver(receiver, FIRST_MAC, makePacket(changedHeader, changedOffer.data(), changedOffer.size()), 20);
  EXPECT_EQ(receiver.state(), NearbySyncExchange::State::Error);
  EXPECT_EQ(receiver.error(), NearbySyncExchange::Error::Protocol);
}

TEST(NearbySyncExchange, DoesNotAckUntilReceiverExplicitlyAppliesTheOffer) {
  NearbySyncExchange sender;
  NearbySyncExchange receiver;
  pairReaders(sender, receiver);
  confirmAndDeliverOffer(sender, receiver);

  NearbySyncRadio::TestPacket ack;
  EXPECT_FALSE(tryTakePacket(receiver, NearbySync::PacketType::Ack, ack));
  sender.update(100);
  EXPECT_EQ(sender.state(), NearbySyncExchange::State::WaitingForAck);
  ASSERT_TRUE(receiver.acknowledgePeerOffer(101));
  EXPECT_TRUE(tryTakePacket(receiver, NearbySync::PacketType::Ack, ack));
}
