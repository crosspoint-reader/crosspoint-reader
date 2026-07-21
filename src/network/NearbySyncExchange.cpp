#include "NearbySyncExchange.h"

#include <Logging.h>

#include <algorithm>
#include <cstring>

#ifndef SIMULATOR
#include <esp_random.h>
#include <esp_system.h>
#endif

namespace {
constexpr char LOG_TAG[] = "CVNEAR";

uint32_t newSessionId() {
#ifdef SIMULATOR
  static uint32_t next = 0x43560001U;
  return next++;
#else
  uint32_t session = 0;
  while (session == 0) session = esp_random();
  return session;
#endif
}
}  // namespace

NearbySyncExchange::NearbySyncExchange()
#ifndef SIMULATOR
    : eventMutex_(xSemaphoreCreateMutex())
#endif
{
}

NearbySyncExchange::~NearbySyncExchange() {
  stop();
#ifndef SIMULATOR
  if (eventMutex_) {
    vSemaphoreDelete(eventMutex_);
    eventMutex_ = nullptr;
  }
#endif
}

bool NearbySyncExchange::start(const NearbySync::Kind kind, const NearbySync::Role role,
                               const NearbySync::MacAddress& localMac, const std::string& localDeviceName,
                               const uint8_t* localOffer, const size_t localOfferSize, const uint32_t nowMs) {
  stop();
  const bool sender = role == NearbySync::Role::Sender;
  if ((kind != NearbySync::Kind::Position && kind != NearbySync::Kind::Stats) ||
      (!sender && role != NearbySync::Role::Receiver) ||
      (sender && (!localOffer || localOfferSize == 0 || localOfferSize > localOffer_.size())) ||
      (!sender && (localOffer || localOfferSize != 0))) {
    return false;
  }
#ifndef SIMULATOR
  if (!eventMutex_) return false;
#endif

  kind_ = kind;
  role_ = role;
  localMac_ = localMac;
  localDeviceName_ = localDeviceName.empty() || localDeviceName.size() > NearbySync::MAX_DEVICE_NAME_BYTES
                         ? "CrossVi"
                         : localDeviceName;
  if (std::any_of(localDeviceName_.begin(), localDeviceName_.end(),
                  [](const unsigned char value) { return value < 0x20 || value == 0x7F; })) {
    localDeviceName_ = "CrossVi";
  }
  if (sender) memcpy(localOffer_.data(), localOffer, localOfferSize);
  localOfferSize_ = localOfferSize;
  localSessionId_ = newSessionId();
  nextSequence_ = 1;
  peer_.reset();
  peerName_.clear();
  peerOfferSize_ = 0;
  pairingConfirmed_ = false;
  localBindSent_ = false;
  peerAcceptedLocalOffer_ = false;
  lastHelloMs_ = 0;
  lastBindMs_ = 0;
  lastOfferMs_ = 0;
  lastAckMs_ = 0;
  lastCompleteMs_ = 0;
  acceptedMs_ = 0;
  eventHead_ = 0;
  eventCount_ = 0;
  eventOverflow_ = false;
  error_ = Error::None;

  if (!radio_.start(this, receiveCallback)) {
    fail(Error::RadioStart);
    return false;
  }
  enter(State::Discovering, nowMs);
  if (!sendHello(nowMs)) {
    fail(Error::Protocol);
    return false;
  }
  return true;
}

void NearbySyncExchange::update(const uint32_t nowMs) {
  if (state_ == State::Idle || state_ == State::Error) return;
  processEvents(nowMs);
  if (state_ == State::Error) return;
  if (state_ == State::Accepted) {
    if (role_ == NearbySync::Role::Sender && nowMs - lastCompleteMs_ >= COMPLETE_INTERVAL_MS) sendComplete(nowMs);
    return;
  }

  const uint32_t elapsed = nowMs - stateStartedMs_;
  if (state_ == State::Discovering) {
    if (elapsed >= DISCOVERY_TIMEOUT_MS) {
      fail(peer_.isBound() ? Error::PairingTimeout : Error::NoPeer);
    } else if (peer_.isBound() && nowMs - lastBindMs_ >= BIND_INTERVAL_MS) {
      sendBind(nowMs);
    } else if (!peer_.isBound() && nowMs - lastHelloMs_ >= HELLO_INTERVAL_MS) {
      sendHello(nowMs);
    }
    return;
  }
  if (state_ == State::Pairing) {
    // Pairing is human-paced. Back is the explicit bounded exit; a transport
    // timer must not expire while someone is still comparing the code.
    if (nowMs - lastBindMs_ >= BIND_INTERVAL_MS) sendBind(nowMs);
    return;
  }
  if ((state_ == State::WaitingForAck || state_ == State::WaitingForOffer || state_ == State::WaitingForComplete) &&
      elapsed >= TRANSFER_TIMEOUT_MS) {
    fail(Error::TransferTimeout);
    return;
  }
  if (state_ == State::WaitingForAck) {
    if (nowMs - lastBindMs_ >= BIND_INTERVAL_MS) sendBind(nowMs);
    if (!peerAcceptedLocalOffer_ && nowMs - lastOfferMs_ >= OFFER_INTERVAL_MS) sendOffer(nowMs);
    return;
  }
  if (state_ == State::WaitingForOffer || state_ == State::OfferReady) {
    if (nowMs - lastBindMs_ >= BIND_INTERVAL_MS) sendBind(nowMs);
    return;
  }
  if (state_ == State::WaitingForComplete) {
    if (nowMs - lastAckMs_ >= ACK_INTERVAL_MS) sendAck(nowMs);
    return;
  }
}

bool NearbySyncExchange::confirmPairing(const uint32_t nowMs) {
  if (state_ != State::Pairing || !peer_.isBound()) return false;
  pairingConfirmed_ = true;
  if (role_ == NearbySync::Role::Sender) {
    if (!sendOffer(nowMs)) {
      fail(Error::Protocol);
      return false;
    }
    enter(State::WaitingForAck, nowMs);
  } else {
    enter(State::WaitingForOffer, nowMs);
  }
  return true;
}

bool NearbySyncExchange::acknowledgePeerOffer(const uint32_t nowMs) {
  if (role_ != NearbySync::Role::Receiver || state_ != State::OfferReady || peerOfferSize_ == 0 || !sendAck(nowMs)) {
    return false;
  }
  enter(State::WaitingForComplete, nowMs);
  return true;
}

void NearbySyncExchange::stop() {
  radio_.stop();
  state_ = State::Idle;
}

uint16_t NearbySyncExchange::pairingCode() const {
  return peer_.isBound() ? NearbySync::pairingCode(localSessionId_, localMac_, peer_.sessionId(), peer_.deviceMac())
                         : 0;
}

bool NearbySyncExchange::readyToExit(const uint32_t nowMs) const {
  if (state_ != State::Accepted) return false;
  return nowMs - acceptedMs_ >= ACCEPT_SETTLE_MS;
}

void NearbySyncExchange::receiveCallback(void* context, const NearbySync::MacAddress& sourceMac, const uint8_t* data,
                                         const size_t size) {
  if (context) static_cast<NearbySyncExchange*>(context)->enqueue(sourceMac, data, size);
}

void NearbySyncExchange::enqueue(const NearbySync::MacAddress& sourceMac, const uint8_t* data, const size_t size) {
  if (!data || size == 0 || size > NearbySync::MAX_PACKET_BYTES) return;
#ifdef SIMULATOR
  (void)sourceMac;
#else
  if (!eventMutex_ || xSemaphoreTake(eventMutex_, 0) != pdTRUE) return;
  if (eventCount_ >= events_.size()) {
    eventOverflow_ = true;
    xSemaphoreGive(eventMutex_);
    return;
  }
  const uint8_t tail = static_cast<uint8_t>((eventHead_ + eventCount_) % events_.size());
  events_[tail].sourceMac = sourceMac;
  memcpy(events_[tail].data.data(), data, size);
  events_[tail].size = static_cast<uint16_t>(size);
  ++eventCount_;
  xSemaphoreGive(eventMutex_);
#endif
}

void NearbySyncExchange::processEvents(const uint32_t nowMs) {
#ifdef SIMULATOR
  (void)nowMs;
#else
  while (true) {
    RawEvent event;
    bool haveEvent = false;
    bool overflow = false;
    if (xSemaphoreTake(eventMutex_, 0) == pdTRUE) {
      overflow = eventOverflow_;
      eventOverflow_ = false;
      if (eventCount_ > 0) {
        event = events_[eventHead_];
        eventHead_ = static_cast<uint8_t>((eventHead_ + 1) % events_.size());
        --eventCount_;
        haveEvent = true;
      }
      xSemaphoreGive(eventMutex_);
    }
    if (overflow) {
      fail(Error::QueueOverflow);
      return;
    }
    if (!haveEvent) return;
    handlePacket(event.sourceMac, event.data.data(), event.size, nowMs);
    if (state_ == State::Error) return;
  }
#endif
}

void NearbySyncExchange::handlePacket(const NearbySync::MacAddress& sourceMac, const uint8_t* data, const size_t size,
                                      const uint32_t nowMs) {
  if (state_ == State::Idle || state_ == State::Error) return;
  NearbySync::DecodedPacket packet;
  if (NearbySync::decodePacket(data, size, packet) != NearbySync::DecodeResult::Ok || packet.header.kind != kind_ ||
      packet.header.role == role_ ||
      (packet.header.senderMac == localMac_ && packet.header.sessionId == localSessionId_)) {
    return;
  }

  if (packet.header.type == NearbySync::PacketType::Hello) {
    if (packet.payloadSize != 0) return;
    if (!peer_.isBound()) {
      if (state_ != State::Discovering || !peer_.bind(sourceMac, packet.header) || !radio_.addPeer(sourceMac)) return;
    } else if (!peer_.accept(sourceMac, packet.header)) {
      return;
    }
    if (state_ == State::Discovering && !sendBind(nowMs)) fail(Error::Protocol);
    return;
  }

  if (packet.header.type == NearbySync::PacketType::Bind) {
    NearbySync::BindPayload bind;
    if (!NearbySync::decodeBindPayload(packet.payload, packet.payloadSize, bind) ||
        bind.targetSessionId != localSessionId_ || bind.targetDeviceMac != localMac_) {
      return;
    }
    const bool wasBound = peer_.isBound();
    if ((!wasBound && (!peer_.bind(sourceMac, packet.header) || !radio_.addPeer(sourceMac))) ||
        (wasBound && !peer_.accept(sourceMac, packet.header))) {
      return;
    }
    peerName_ = std::move(bind.deviceName);
    if (!localBindSent_ && !sendBind(nowMs)) {
      fail(Error::Protocol);
      return;
    }
    if (state_ == State::Discovering && localBindSent_) enter(State::Pairing, nowMs);
    return;
  }

  if (packet.header.type == NearbySync::PacketType::Offer) {
    if (role_ != NearbySync::Role::Receiver ||
        (state_ != State::WaitingForOffer && state_ != State::OfferReady && state_ != State::WaitingForComplete &&
         state_ != State::Accepted) ||
        !peer_.accept(sourceMac, packet.header)) {
      return;
    }
    if (packet.payloadSize == 0 || packet.payloadSize > peerOffer_.size()) {
      fail(Error::Protocol);
      return;
    }
    // Retransmission is expected, mutation is not. Once an offer has been
    // exposed to the activity for validation/display, pin its exact bytes for
    // the rest of the bound session so Confirm can never save/apply one payload
    // while acknowledging another.
    if (peerOfferSize_ > 0 &&
        (packet.payloadSize != peerOfferSize_ || memcmp(peerOffer_.data(), packet.payload, peerOfferSize_) != 0)) {
      fail(Error::Protocol);
      return;
    }
    memcpy(peerOffer_.data(), packet.payload, packet.payloadSize);
    peerOfferSize_ = packet.payloadSize;
    if (state_ == State::WaitingForOffer) enter(State::OfferReady, nowMs);
    if (state_ == State::WaitingForComplete || state_ == State::Accepted) sendAck(nowMs);
    return;
  }
  if (packet.header.type == NearbySync::PacketType::Ack) {
    NearbySync::AckPayload ack;
    if (role_ != NearbySync::Role::Sender || (state_ != State::WaitingForAck && state_ != State::Accepted) ||
        !NearbySync::decodeAckPayload(packet.payload, packet.payloadSize, ack) ||
        ack.targetSessionId != localSessionId_ || ack.targetDeviceMac != localMac_ ||
        !peer_.accept(sourceMac, packet.header)) {
      return;
    }
    peerAcceptedLocalOffer_ = true;
    if (state_ == State::WaitingForAck) {
      acceptedMs_ = nowMs;
      enter(State::Accepted, nowMs);
    }
    sendComplete(nowMs);
    return;
  }
  if (packet.header.type == NearbySync::PacketType::Complete) {
    NearbySync::AckPayload completion;
    if (role_ != NearbySync::Role::Receiver || (state_ != State::WaitingForComplete && state_ != State::Accepted) ||
        !NearbySync::decodeAckPayload(packet.payload, packet.payloadSize, completion) ||
        completion.targetSessionId != localSessionId_ || completion.targetDeviceMac != localMac_ ||
        !peer_.accept(sourceMac, packet.header)) {
      return;
    }
    if (state_ == State::WaitingForComplete) {
      acceptedMs_ = nowMs;
      enter(State::Accepted, nowMs);
    }
  }
}

bool NearbySyncExchange::sendPacket(const NearbySync::PacketType type, const NearbySync::MacAddress& destination,
                                    const uint8_t* payload, const size_t payloadSize) {
  std::array<uint8_t, NearbySync::MAX_PACKET_BYTES> packet{};
  size_t packetSize = 0;
  const uint16_t sequence = nextSequence_++;
  if (nextSequence_ == 0) nextSequence_ = 1;
  const NearbySync::PacketHeader header{kind_, type, role_, localSessionId_, sequence, localMac_};
  return NearbySync::encodePacket(header, payload, payloadSize, packet.data(), packet.size(), packetSize) &&
         radio_.send(destination, packet.data(), packetSize);
}

bool NearbySyncExchange::sendHello(const uint32_t nowMs) {
  lastHelloMs_ = nowMs;
  return sendPacket(NearbySync::PacketType::Hello, BROADCAST_MAC, nullptr, 0);
}

bool NearbySyncExchange::sendBind(const uint32_t nowMs) {
  if (!peer_.isBound()) return false;
  std::array<uint8_t, NearbySync::MAX_PAYLOAD_BYTES> payload{};
  size_t payloadSize = 0;
  const NearbySync::BindPayload bind{peer_.sessionId(), peer_.deviceMac(), localDeviceName_};
  lastBindMs_ = nowMs;
  const bool sent = NearbySync::encodeBindPayload(bind, payload.data(), payload.size(), payloadSize) &&
                    sendPacket(NearbySync::PacketType::Bind, peer_.transportMac(), payload.data(), payloadSize);
  localBindSent_ = localBindSent_ || sent;
  return sent;
}

bool NearbySyncExchange::sendOffer(const uint32_t nowMs) {
  if (role_ != NearbySync::Role::Sender || !peer_.isBound() || !pairingConfirmed_ || localOfferSize_ == 0) return false;
  lastOfferMs_ = nowMs;
  return sendPacket(NearbySync::PacketType::Offer, peer_.transportMac(), localOffer_.data(), localOfferSize_);
}

bool NearbySyncExchange::sendAck(const uint32_t nowMs) {
  if (role_ != NearbySync::Role::Receiver || !peer_.isBound() || peerOfferSize_ == 0) return false;
  std::array<uint8_t, 10> payload{};
  size_t payloadSize = 0;
  const NearbySync::AckPayload ack{peer_.sessionId(), peer_.deviceMac()};
  lastAckMs_ = nowMs;
  return NearbySync::encodeAckPayload(ack, payload.data(), payload.size(), payloadSize) &&
         sendPacket(NearbySync::PacketType::Ack, peer_.transportMac(), payload.data(), payloadSize);
}

bool NearbySyncExchange::sendComplete(const uint32_t nowMs) {
  if (role_ != NearbySync::Role::Sender || !peer_.isBound() || !peerAcceptedLocalOffer_) return false;
  std::array<uint8_t, 10> payload{};
  size_t payloadSize = 0;
  const NearbySync::AckPayload completion{peer_.sessionId(), peer_.deviceMac()};
  lastCompleteMs_ = nowMs;
  return NearbySync::encodeAckPayload(completion, payload.data(), payload.size(), payloadSize) &&
         sendPacket(NearbySync::PacketType::Complete, peer_.transportMac(), payload.data(), payloadSize);
}

void NearbySyncExchange::enter(const State state, const uint32_t nowMs) {
  state_ = state;
  stateStartedMs_ = nowMs;
}

void NearbySyncExchange::fail(const Error error) {
  LOG_ERR(LOG_TAG, "Nearby exchange failed: %u", static_cast<unsigned>(error));
  error_ = error;
  state_ = State::Error;
  radio_.stop();
}
