#include "TerminalProtocol.h"

namespace knietty {

bool isKnownFrameType(const uint8_t type) {
  switch (static_cast<FrameType>(type)) {
    case FrameType::TerminalOutput:
    case FrameType::TerminalInput:
    case FrameType::ControlRequest:
    case FrameType::ControlResponse:
    case FrameType::RefreshEvent:
    case FrameType::Heartbeat:
    case FrameType::SessionEnd:
    case FrameType::TerminalOutputEnd:
      return true;
  }
  return false;
}

bool isOptionalFrameType(const uint8_t type) { return (type & OPTIONAL_TYPE_MASK) != 0; }

void encodeFrameHeader(uint8_t* output, const uint8_t type, const uint8_t flags, const uint16_t length,
                       const uint32_t sequence) {
  if (output == nullptr) return;
  output[0] = type;
  output[1] = flags;
  output[2] = static_cast<uint8_t>(length >> 8);
  output[3] = static_cast<uint8_t>(length);
  output[4] = static_cast<uint8_t>(sequence >> 24);
  output[5] = static_cast<uint8_t>(sequence >> 16);
  output[6] = static_cast<uint8_t>(sequence >> 8);
  output[7] = static_cast<uint8_t>(sequence);
}

FrameDecoder::FeedResult FrameDecoder::decodeHeader() {
  if (header[1] != 0) {
    error = FrameError::InvalidFlags;
    return FeedResult::Error;
  }
  payloadLength = static_cast<size_t>((static_cast<uint16_t>(header[2]) << 8) | header[3]);
  if (payloadLength > payload.size()) {
    error = FrameError::PayloadTooLarge;
    return FeedResult::Error;
  }
  if (payloadLength == 0) {
    frameReady = true;
    return FeedResult::Ready;
  }
  return FeedResult::NeedMore;
}

FrameDecoder::FeedResult FrameDecoder::feed(const uint8_t byte) {
  if (error != FrameError::None) return FeedResult::Error;
  if (frameReady) {
    error = FrameError::FrameNotConsumed;
    return FeedResult::Error;
  }
  if (headerLength < header.size()) {
    header[headerLength++] = byte;
    return headerLength == header.size() ? decodeHeader() : FeedResult::NeedMore;
  }
  payload[payloadReceived++] = byte;
  if (payloadReceived == payloadLength) {
    frameReady = true;
    return FeedResult::Ready;
  }
  return FeedResult::NeedMore;
}

size_t FrameDecoder::bytesNeeded() const {
  if (error != FrameError::None || frameReady) return 0;
  if (headerLength < header.size()) return header.size() - headerLength;
  return payloadLength - payloadReceived;
}

FrameView FrameDecoder::frame() const {
  if (!frameReady) return {};
  const uint32_t sequence = (static_cast<uint32_t>(header[4]) << 24) | (static_cast<uint32_t>(header[5]) << 16) |
                            (static_cast<uint32_t>(header[6]) << 8) | header[7];
  return {header[0], header[1], static_cast<uint16_t>(payloadLength), sequence, payload.data()};
}

void FrameDecoder::consume() {
  if (!frameReady) return;
  headerLength = 0;
  payloadLength = 0;
  payloadReceived = 0;
  frameReady = false;
}

void FrameDecoder::reset() {
  headerLength = 0;
  payloadLength = 0;
  payloadReceived = 0;
  frameReady = false;
  error = FrameError::None;
}

}  // namespace knietty
