#include "TerminalWifi.h"

#include <ESPmDNS.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_mac.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace {

constexpr char HELLO_V1_PREFIX[] = "KNIETTY/1 HELLO ";
constexpr char HELLO_V2_PREFIX[] = "KNIETTY/2 HELLO ";
constexpr char HELLO_V3_PREFIX[] = "KNIETTY/3 HELLO ";
constexpr char RESPONSE_ACCEPT_FORMAT[] = "KNIETTY/%u ACCEPT %u %u\n";
constexpr char RESPONSE_ACCEPT_V3_FORMAT[] = "KNIETTY/3 ACCEPT %u %u %s\n";
constexpr char RESPONSE_DENY[] = "KNIETTY/1 DENY\n";
constexpr char RESPONSE_ERROR[] = "KNIETTY/1 ERROR\n";
constexpr char DISCOVERY_REQUEST[] = "KNIETTY/1 DISCOVER";
constexpr char DISCOVERY_RESPONSE_FORMAT[] = "KNIETTY/1 HERE %s %u tls=required\n";
constexpr char CAPABILITY_FRAME[] = "frame";
constexpr char CAPABILITY_TERMINAL[] = "frame,burst1";
constexpr char CAPABILITY_DIAGNOSTICS[] = "frame,diag1";

char* takeToken(char*& cursor) {
  if (cursor == nullptr || *cursor == '\0') return nullptr;
  char* token = cursor;
  char* separator = std::strchr(cursor, ' ');
  if (separator == nullptr) return nullptr;
  *separator = '\0';
  cursor = separator + 1;
  return token;
}

}  // namespace

void TerminalWifi::setState(const State next) {
  if (state == next) return;
  state = next;
  ++generation;
}

void TerminalWifi::begin() {
  active = true;
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);

  uint8_t mac[6]{};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
    std::snprintf(hostname, sizeof(hostname), "knietty-%02x%02x%02x", mac[3], mac[4], mac[5]);
  } else {
    std::snprintf(hostname, sizeof(hostname), "knietty-x4");
  }

  if (!tls.begin(hostname)) {
    LOG_ERR("KNIETTY", "Could not initialize unique TLS identity");
    active = false;
    setState(State::Offline);
    return;
  }

  poll();
}

void TerminalWifi::end() {
  active = false;
  notifySessionEnd();
  stopService();
  tls.end();
  WiFi.setSleep(true);
}

void TerminalWifi::startService() {
  if (serviceStarted || WiFi.status() != WL_CONNECTED) return;

  server.begin();
  server.setNoDelay(true);
  serviceStarted = static_cast<bool>(server);
  if (!serviceStarted) {
    setState(State::Offline);
    LOG_ERR("KNIETTY", "Could not start TCP server on port %u", PORT);
    return;
  }

  const IPAddress address = WiFi.localIP();
  std::snprintf(localIp, sizeof(localIp), "%u.%u.%u.%u", address[0], address[1], address[2], address[3]);

  MDNS.end();
  mdnsStarted = MDNS.begin(hostname);
  if (mdnsStarted) {
    MDNS.addService("knietty", "tcp", PORT);
    MDNS.addServiceTxt("knietty", "tcp", "proto", "3");
    MDNS.addServiceTxt("knietty", "tcp", "id", static_cast<const char*>(hostname));
    MDNS.addServiceTxt("knietty", "tcp", "cols", "80");
    MDNS.addServiceTxt("knietty", "tcp", "rows", "24");
    MDNS.addServiceTxt("knietty", "tcp", "approval", "required");
    MDNS.addServiceTxt("knietty", "tcp", "tls", "required");
    MDNS.addServiceTxt("knietty", "tcp", "fingerprint", tls.deviceFingerprintText());
  } else {
    LOG_ERR("KNIETTY", "Could not start mDNS; explicit IP connections remain available");
  }
  discoveryStarted = discovery.begin(PORT) != 0;
  if (!discoveryStarted) LOG_ERR("KNIETTY", "Could not start UDP discovery on port %u", PORT);

  setState(State::Waiting);
}

void TerminalWifi::stopService() {
  disconnectClient();
  if (serviceStarted) {
    server.end();
    serviceStarted = false;
  }
  if (mdnsStarted) {
    MDNS.end();
    mdnsStarted = false;
  }
  if (discoveryStarted) {
    discovery.stop();
    discoveryStarted = false;
  }
  localIp[0] = '\0';
  setState(State::Offline);
}

void TerminalWifi::notifySessionEnd() {
  if (state != State::Connected || helloVersion != 3 || !tls.connected()) return;

  // Terminal input still queued when the user exits is no longer useful. Drop
  // it so the close notification cannot be trapped behind a wrapped ring or a
  // full socket during the immediately following Wi-Fi teardown.
  txHead = 0;
  txSize = 0;
  uint8_t header[knietty::FRAME_HEADER_SIZE];
  knietty::encodeFrameHeader(header, static_cast<uint8_t>(knietty::FrameType::SessionEnd), 0, 0, nextTxSequence++);
  if (tls.write(header, sizeof(header)) == sizeof(header)) {
    // NetworkClient::flush() clears RX rather than flushing TX. A short grace
    // period lets lwIP put this eight-byte frame on the WLAN before stop().
    delay(SESSION_END_GRACE_MS);
  }
}

void TerminalWifi::disconnectClient() {
  tls.stop();
  helloLength = 0;
  helloBuffer[0] = '\0';
  clientName[0] = '\0';
  clientIp[0] = '\0';
  hostEpochSeconds = 0;
  hostUtcOffsetMinutes = 0;
  hostTimeCapturedAt = 0;
  helloVersion = 1;
  sessionMode = Mode::Terminal;
  hasHostTime = false;
  frameDecoder.reset();
  frameReadOffset = 0;
  txHead = 0;
  txSize = 0;
  nextTxSequence = 1;
  pairCommitPending = false;
  pendingOutputBurstEnds = 0;
}

void TerminalWifi::acceptIncoming() {
  NetworkClient incoming = server.accept();
  if (!incoming) return;

  if (state != State::Waiting || static_cast<int32_t>(millis() - nextAcceptAt) < 0) {
    incoming.stop();
    return;
  }

  const IPAddress remote = incoming.remoteIP();
  std::snprintf(clientIp, sizeof(clientIp), "%u.%u.%u.%u", remote[0], remote[1], remote[2], remote[3]);
  if (!tls.attach(std::move(incoming))) {
    nextAcceptAt = millis() + 1000;
    return;
  }
  helloLength = 0;
  helloBuffer[0] = '\0';
  helloDeadline = millis() + 10000;
  setState(State::TlsNegotiating);
}

void TerminalWifi::pollTlsHandshake() {
  const auto result = tls.pollHandshake();
  if (result == TerminalTls::HandshakeResult::Pending && static_cast<int32_t>(millis() - helloDeadline) < 0) return;
  if (result != TerminalTls::HandshakeResult::Connected) {
    disconnectClient();
    nextAcceptAt = millis() + 1000;
    setState(State::Waiting);
    return;
  }
  helloDeadline = millis() + HELLO_TIMEOUT_MS;
  setState(State::Negotiating);
}

void TerminalWifi::pollDiscovery() {
  if (!discoveryStarted || discovery.parsePacket() <= 0) return;

  char request[32];
  int length = discovery.read(request, sizeof(request) - 1);
  if (length <= 0) return;
  request[length] = '\0';
  while (length > 0 && (request[length - 1] == '\r' || request[length - 1] == '\n')) {
    request[--length] = '\0';
  }
  if (std::strcmp(request, DISCOVERY_REQUEST) != 0) return;

  char response[96];
  const int responseLength = std::snprintf(response, sizeof(response), DISCOVERY_RESPONSE_FORMAT, hostname, PORT);
  if (responseLength <= 0 || responseLength >= static_cast<int>(sizeof(response))) return;
  if (!discovery.beginPacket(discovery.remoteIP(), discovery.remotePort())) return;
  discovery.write(reinterpret_cast<const uint8_t*>(response), static_cast<size_t>(responseLength));
  discovery.endPacket();
}

bool TerminalWifi::parseHello() {
  const char* nameStart = nullptr;
  if (std::strncmp(helloBuffer, HELLO_V3_PREFIX, sizeof(HELLO_V3_PREFIX) - 1) == 0) {
    helloVersion = 3;
    char* cursor = helloBuffer + sizeof(HELLO_V3_PREFIX) - 1;
    const char* mode = takeToken(cursor);
    const char* capabilities = takeToken(cursor);
    const char* epochText = takeToken(cursor);
    const char* offsetText = takeToken(cursor);
    if (mode == nullptr || capabilities == nullptr || epochText == nullptr || offsetText == nullptr) {
      return false;
    }
    if (std::strcmp(mode, "terminal") == 0 && std::strcmp(capabilities, CAPABILITY_FRAME) == 0) {
      sessionMode = Mode::Terminal;
    } else if (std::strcmp(mode, "diagnostics") == 0 && std::strcmp(capabilities, CAPABILITY_DIAGNOSTICS) == 0) {
      sessionMode = Mode::Diagnostics;
    } else {
      return false;
    }
    char* end = nullptr;
    const unsigned long long epoch = std::strtoull(epochText, &end, 10);
    if (end == epochText || *end != '\0') return false;
    const long offset = std::strtol(offsetText, &end, 10);
    if (end == offsetText || *end != '\0' || epoch < 946684800ULL || epoch > 4102444800ULL || offset < -840 ||
        offset > 840 || *cursor == '\0') {
      return false;
    }
    nameStart = cursor;
    hostEpochSeconds = static_cast<uint64_t>(epoch);
    hostUtcOffsetMinutes = static_cast<int16_t>(offset);
    hostTimeCapturedAt = millis();
    hasHostTime = true;
  } else if (std::strncmp(helloBuffer, HELLO_V2_PREFIX, sizeof(HELLO_V2_PREFIX) - 1) == 0) {
#ifdef KNIETTY_TLS
    return false;
#else
    helloVersion = 2;
    char* cursor = helloBuffer + sizeof(HELLO_V2_PREFIX) - 1;
    char* end = nullptr;
    const unsigned long long epoch = std::strtoull(cursor, &end, 10);
    if (end == cursor || *end != ' ') return false;
    cursor = end + 1;
    const long offset = std::strtol(cursor, &end, 10);
    if (end == cursor || *end != ' ' || epoch < 946684800ULL || epoch > 4102444800ULL || offset < -840 ||
        offset > 840) {
      return false;
    }
    nameStart = end + 1;
    hostEpochSeconds = static_cast<uint64_t>(epoch);
    hostUtcOffsetMinutes = static_cast<int16_t>(offset);
    hostTimeCapturedAt = millis();
    hasHostTime = true;
#endif
  } else if (std::strncmp(helloBuffer, HELLO_V1_PREFIX, sizeof(HELLO_V1_PREFIX) - 1) == 0) {
#ifdef KNIETTY_TLS
    return false;
#else
    helloVersion = 1;
    sessionMode = Mode::Terminal;
    nameStart = helloBuffer + sizeof(HELLO_V1_PREFIX) - 1;
    hasHostTime = false;
#endif
  } else {
    return false;
  }

  size_t output = 0;
  for (const char* input = nameStart; *input != '\0' && output + 1 < sizeof(clientName); ++input) {
    const unsigned char byte = static_cast<unsigned char>(*input);
    if (byte == '\r' || byte == '\n') break;
    if (std::isalnum(byte) || byte == ' ' || byte == '-' || byte == '_' || byte == '.') {
      clientName[output++] = static_cast<char>(byte);
    } else {
      clientName[output++] = '?';
    }
  }
  clientName[output] = '\0';
  return output != 0;
}

void TerminalWifi::pollHandshake() {
  while (tls.available() > 0 && helloLength + 1 < sizeof(helloBuffer)) {
    const int byte = tls.read();
    if (byte < 0) break;
    helloBuffer[helloLength++] = static_cast<char>(byte);
    helloBuffer[helloLength] = '\0';
    if (byte == '\n') {
      if (parseHello()) {
        if (tls.peerNameChanged(clientName)) {
          tls.write(reinterpret_cast<const uint8_t*>(RESPONSE_DENY), sizeof(RESPONSE_DENY) - 1);
          disconnectClient();
          nextAcceptAt = millis() + 1000;
          setState(State::Waiting);
        } else if (sessionMode == Mode::Terminal && tls.peerIsPaired()) {
          setState(State::ApprovalPending);
          acceptRequest(80, 24);
        } else {
          setState(State::ApprovalPending);
        }
      } else {
        tls.write(reinterpret_cast<const uint8_t*>(RESPONSE_ERROR), sizeof(RESPONSE_ERROR) - 1);
        disconnectClient();
        setState(State::Waiting);
      }
      return;
    }
  }

  if (helloLength + 1 >= sizeof(helloBuffer) || static_cast<int32_t>(millis() - helloDeadline) >= 0) {
    tls.write(reinterpret_cast<const uint8_t*>(RESPONSE_ERROR), sizeof(RESPONSE_ERROR) - 1);
    disconnectClient();
    setState(State::Waiting);
  }
}

void TerminalWifi::poll() {
  if (!active) return;
  if (WiFi.status() != WL_CONNECTED) {
    if (serviceStarted) stopService();
    return;
  }
  if (!serviceStarted) startService();
  if (!serviceStarted) return;

  if (state != State::Waiting && !tls.connected()) {
    disconnectClient();
    setState(State::Waiting);
  }

  acceptIncoming();
  pollDiscovery();
  if (state == State::TlsNegotiating) pollTlsHandshake();
  if (state == State::Negotiating) pollHandshake();
  if (state == State::Connected && helloVersion == 3) {
    flushTx();
    pollFramedClient();
  }
}

void TerminalWifi::protocolError() {
  disconnectClient();
  setState(State::Waiting);
}

void TerminalWifi::pollFramedClient() {
  if (!isConnected() || helloVersion != 3) return;
  uint8_t received[128];
  for (;;) {
    if (frameDecoder.hasFrame()) {
      const knietty::FrameView frame = frameDecoder.frame();
      if (pairCommitPending) {
        if (frame.type != static_cast<uint8_t>(knietty::FrameType::Heartbeat) || frame.length != 0 ||
            !tls.trustPeer(clientName)) {
          protocolError();
          return;
        }
        pairCommitPending = false;
        frameDecoder.consume();
        frameReadOffset = 0;
        continue;
      }
      if (frame.type == static_cast<uint8_t>(knietty::FrameType::TerminalOutputEnd)) {
        if (sessionMode != Mode::Terminal || frame.length != 0) {
          protocolError();
          return;
        }
        if (pendingOutputBurstEnds != UINT8_MAX) ++pendingOutputBurstEnds;
        frameDecoder.consume();
        frameReadOffset = 0;
        continue;
      }
      if (knietty::isOptionalFrameType(frame.type) ||
          frame.type == static_cast<uint8_t>(knietty::FrameType::Heartbeat)) {
        frameDecoder.consume();
        frameReadOffset = 0;
        continue;
      }
      if (frame.type == static_cast<uint8_t>(knietty::FrameType::TerminalOutput) && sessionMode == Mode::Terminal) {
        if (frame.length == 0) {
          frameDecoder.consume();
          frameReadOffset = 0;
          continue;
        }
        return;
      }
      if (frame.type == static_cast<uint8_t>(knietty::FrameType::ControlRequest)) {
        return;
      }
      protocolError();
      return;
    }
    if (tls.available() <= 0) return;
    const size_t wanted = std::min(frameDecoder.bytesNeeded(), sizeof(received));
    if (wanted == 0) {
      protocolError();
      return;
    }
    const int count = tls.read(received, wanted);
    if (count <= 0) return;
    for (int index = 0; index < count; ++index) {
      const auto result = frameDecoder.feed(received[index]);
      if (result == knietty::FrameDecoder::FeedResult::Error) {
        protocolError();
        return;
      }
    }
  }
}

int TerminalWifi::available() {
  if (!isConnected()) return 0;
  if (helloVersion != 3) return tls.available();
  pollFramedClient();
  if (!isConnected() || !frameDecoder.hasFrame()) return 0;
  const knietty::FrameView frame = frameDecoder.frame();
  if (frame.type != static_cast<uint8_t>(knietty::FrameType::TerminalOutput) || frameReadOffset >= frame.length)
    return 0;
  return static_cast<int>(frame.length - frameReadOffset);
}

int TerminalWifi::read() {
  uint8_t byte = 0;
  return read(&byte, 1) == 1 ? byte : -1;
}

int TerminalWifi::read(uint8_t* output, const size_t length) {
  if (!isConnected() || output == nullptr || length == 0) return -1;
  if (helloVersion != 3) return tls.read(output, length);
  if (available() <= 0) return 0;
  const knietty::FrameView frame = frameDecoder.frame();
  const size_t count = std::min(length, static_cast<size_t>(frame.length) - frameReadOffset);
  std::memcpy(output, frame.payload + frameReadOffset, count);
  frameReadOffset += count;
  if (frameReadOffset == frame.length) {
    frameDecoder.consume();
    frameReadOffset = 0;
  }
  return static_cast<int>(count);
}

uint8_t TerminalWifi::takeOutputBurstEnds() {
  const uint8_t count = pendingOutputBurstEnds;
  pendingOutputBurstEnds = 0;
  return count;
}

size_t TerminalWifi::write(const uint8_t byte) { return write(&byte, 1); }

size_t TerminalWifi::write(const uint8_t* data, const size_t length) {
  if (!isConnected() || data == nullptr || length == 0) return 0;
  if (helloVersion != 3) return tls.write(data, length);
  if (length > knietty::MAX_FRAME_PAYLOAD || sessionMode != Mode::Terminal) return 0;
  const uint32_t sequence = nextTxSequence;
  if (!queueFrame(knietty::FrameType::TerminalInput, data, length, sequence)) return 0;
  nextTxSequence = nextTxSequence + 1;
  flushTx();
  return length;
}

bool TerminalWifi::takeControlRequest(uint8_t* output, const size_t capacity, size_t& length, uint32_t& sequence) {
  length = 0;
  sequence = 0;
  if (!isConnected() || helloVersion != 3) return false;
  pollFramedClient();
  if (!isConnected() || !frameDecoder.hasFrame()) return false;
  const knietty::FrameView frame = frameDecoder.frame();
  if (frame.type != static_cast<uint8_t>(knietty::FrameType::ControlRequest)) return false;
  length = frame.length;
  sequence = frame.sequence;
  const size_t copied = std::min(capacity, static_cast<size_t>(frame.length));
  if (output != nullptr && copied != 0) std::memcpy(output, frame.payload, copied);
  frameDecoder.consume();
  frameReadOffset = 0;
  return true;
}

bool TerminalWifi::sendFrame(const knietty::FrameType type, const uint8_t* payload, const size_t length,
                             const uint32_t sequence) {
  if (!isConnected() || helloVersion != 3) return false;
  if (!queueFrame(type, payload, length, sequence)) return false;
  flushTx();
  return true;
}

void TerminalWifi::abortClient() {
  disconnectClient();
  if (serviceStarted) setState(State::Waiting);
}

bool TerminalWifi::enqueueTx(const uint8_t* data, const size_t length) {
  if (data == nullptr || length > TX_BUFFER_SIZE - txSize) return false;
  size_t tail = (txHead + txSize) % TX_BUFFER_SIZE;
  for (size_t index = 0; index < length; ++index) {
    txBuffer[tail] = data[index];
    tail = (tail + 1) % TX_BUFFER_SIZE;
  }
  txSize += length;
  return true;
}

bool TerminalWifi::queueFrame(const knietty::FrameType type, const uint8_t* payload, const size_t length,
                              const uint32_t sequence) {
  if ((payload == nullptr && length != 0) || length > knietty::MAX_FRAME_PAYLOAD ||
      knietty::FRAME_HEADER_SIZE + length > TX_BUFFER_SIZE - txSize) {
    return false;
  }
  uint8_t header[knietty::FRAME_HEADER_SIZE];
  knietty::encodeFrameHeader(header, static_cast<uint8_t>(type), 0, static_cast<uint16_t>(length), sequence);
  if (!enqueueTx(header, sizeof(header))) return false;
  if (length != 0 && !enqueueTx(payload, length)) {
    // The combined capacity check above makes this unreachable; keep the queue
    // intact rather than introducing rollback state for a partial frame.
    return false;
  }
  return true;
}

void TerminalWifi::flushTx() {
  if (!isConnected() || txSize == 0) return;
  const size_t contiguous = std::min(txSize, TX_BUFFER_SIZE - txHead);
  const size_t written = tls.write(txBuffer + txHead, contiguous);
  if (written == 0) return;
  txHead = (txHead + written) % TX_BUFFER_SIZE;
  txSize -= written;
}

void TerminalWifi::acceptRequest(const uint8_t columns, const uint8_t rows) {
  if (state != State::ApprovalPending || !tls.connected()) return;
  const bool needsPairCommit = !tls.peerIsPaired();
  if (needsPairCommit && !tls.canTrustPeer()) {
    tls.write(reinterpret_cast<const uint8_t*>(RESPONSE_ERROR), sizeof(RESPONSE_ERROR) - 1);
    disconnectClient();
    setState(State::Waiting);
    return;
  }
  char response[56];
  const int length =
      helloVersion == 3
          ? std::snprintf(response, sizeof(response), RESPONSE_ACCEPT_V3_FORMAT, columns, rows,
                          sessionMode == Mode::Diagnostics ? CAPABILITY_DIAGNOSTICS : CAPABILITY_TERMINAL)
          : std::snprintf(response, sizeof(response), RESPONSE_ACCEPT_FORMAT, helloVersion, columns, rows);
  if (length <= 0 || tls.write(reinterpret_cast<const uint8_t*>(response), static_cast<size_t>(length)) == 0) {
    disconnectClient();
    setState(State::Waiting);
    return;
  }
  pairCommitPending = needsPairCommit;
  setState(State::Connected);
}

bool TerminalWifi::formatHostTime(char* buffer, const size_t bufferSize) const {
  if (!hasHostTime || buffer == nullptr || bufferSize < 6) return false;
  const uint64_t elapsed = static_cast<uint32_t>(millis() - hostTimeCapturedAt) / 1000ULL;
  int64_t localSeconds =
      static_cast<int64_t>(hostEpochSeconds + elapsed) + static_cast<int64_t>(hostUtcOffsetMinutes) * 60;
  localSeconds %= 86400;
  if (localSeconds < 0) localSeconds += 86400;
  const unsigned hour = static_cast<unsigned>(localSeconds / 3600);
  const unsigned minute = static_cast<unsigned>((localSeconds / 60) % 60);
  std::snprintf(buffer, bufferSize, "%02u:%02u", hour, minute);
  return true;
}

void TerminalWifi::denyRequest() {
  if (state != State::ApprovalPending) return;
  tls.write(reinterpret_cast<const uint8_t*>(RESPONSE_DENY), sizeof(RESPONSE_DENY) - 1);
  disconnectClient();
  setState(State::Waiting);
}
