#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace knietty {

constexpr size_t FRAME_HEADER_SIZE = 8;
constexpr size_t MAX_FRAME_PAYLOAD = 512;
constexpr uint8_t OPTIONAL_TYPE_MASK = 0x80;

enum class FrameType : uint8_t {
  TerminalOutput = 0x01,
  TerminalInput = 0x02,
  ControlRequest = 0x03,
  ControlResponse = 0x04,
  RefreshEvent = 0x05,
  Heartbeat = 0x06,
  SessionEnd = 0x07,
  // Optional zero-payload marker sent after the host PTY has been quiet long
  // enough to delimit one output burst. Older peers safely ignore it.
  TerminalOutputEnd = 0x80,
};

enum class FrameError : uint8_t {
  None,
  InvalidFlags,
  PayloadTooLarge,
  FrameNotConsumed,
};

struct FrameView {
  uint8_t type = 0;
  uint8_t flags = 0;
  uint16_t length = 0;
  uint32_t sequence = 0;
  const uint8_t* payload = nullptr;
};

bool isKnownFrameType(uint8_t type);
bool isOptionalFrameType(uint8_t type);
void encodeFrameHeader(uint8_t* output, uint8_t type, uint8_t flags, uint16_t length, uint32_t sequence);

class FrameDecoder {
 public:
  enum class FeedResult : uint8_t { NeedMore, Ready, Error };

  FeedResult feed(uint8_t byte);
  void consume();
  void reset();

  bool hasFrame() const { return frameReady; }
  size_t bytesNeeded() const;
  FrameError getError() const { return error; }
  FrameView frame() const;

 private:
  std::array<uint8_t, FRAME_HEADER_SIZE> header{};
  std::array<uint8_t, MAX_FRAME_PAYLOAD> payload{};
  size_t headerLength = 0;
  size_t payloadLength = 0;
  size_t payloadReceived = 0;
  bool frameReady = false;
  FrameError error = FrameError::None;

  FeedResult decodeHeader();
};

}  // namespace knietty
