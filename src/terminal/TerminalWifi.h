#pragma once

#include <Arduino.h>
#include <NetworkClient.h>
#include <NetworkServer.h>
#include <NetworkUdp.h>

#include <cstddef>
#include <cstdint>

#include "TerminalProtocol.h"
#include "TerminalTls.h"

class TerminalWifi {
 public:
  enum class State : uint8_t { Offline, Waiting, TlsNegotiating, Negotiating, ApprovalPending, Connected };
  enum class Mode : uint8_t { Terminal, Diagnostics };

  static constexpr uint16_t PORT = 29380;

  void begin();
  void end();
  void poll();

  int available();
  int read();
  int read(uint8_t* output, size_t length);
  uint8_t takeOutputBurstEnds();
  size_t write(uint8_t byte);
  size_t write(const uint8_t* data, size_t length);
  bool takeControlRequest(uint8_t* output, size_t capacity, size_t& length, uint32_t& sequence);
  bool sendFrame(knietty::FrameType type, const uint8_t* payload, size_t length, uint32_t sequence);
  void abortClient();

  void acceptRequest(uint8_t columns, uint8_t rows);
  void denyRequest();
  bool forgetAllHosts() { return tls.forgetAllPeers(); }
  bool forgetHost(uint8_t index) { return tls.forgetPeer(index); }
  const char* getPairedHostName(uint8_t index) const { return tls.pairedPeerName(index); }
  bool formatPairedHostFingerprint(uint8_t index, char* output, size_t outputSize) const {
    return tls.formatPairedPeerFingerprint(index, output, outputSize);
  }
  bool formatHostTime(char* buffer, size_t bufferSize) const;

  State getState() const { return state; }
  bool isConnected() { return state == State::Connected && tls.connected(); }
  const char* getClientName() const { return clientName; }
  const char* getClientIp() const { return clientIp; }
  const char* getHostname() const { return hostname; }
  const char* getLocalIp() const { return localIp; }
  uint32_t getGeneration() const { return generation; }
  uint8_t getProtocolVersion() const { return helloVersion; }
  Mode getMode() const { return sessionMode; }
  bool isFramed() const { return helloVersion == 3; }
  bool isPairedHost() const { return tls.peerIsPaired(); }
  uint8_t getPairedHostCount() const { return tls.pairedPeerCount(); }
  const char* getPairingCode() const { return tls.pairingCode(); }
  const char* getDeviceFingerprint() const { return tls.deviceFingerprintText(); }

 private:
  static constexpr uint32_t HELLO_TIMEOUT_MS = 5000;
  static constexpr size_t HELLO_BUFFER_SIZE = 128;
  static constexpr size_t CLIENT_NAME_SIZE = 33;
  static constexpr size_t TX_BUFFER_SIZE = 1024;
  static constexpr uint32_t SESSION_END_GRACE_MS = 25;

  NetworkServer server{PORT, 1};
  TerminalTls tls;
  NetworkUDP discovery;
  State state = State::Offline;
  bool active = false;
  bool serviceStarted = false;
  bool mdnsStarted = false;
  bool discoveryStarted = false;
  uint32_t helloDeadline = 0;
  uint32_t nextAcceptAt = 0;
  uint32_t generation = 0;
  size_t helloLength = 0;
  char helloBuffer[HELLO_BUFFER_SIZE]{};
  char clientName[CLIENT_NAME_SIZE]{};
  char clientIp[16]{};
  char hostname[32]{};
  char localIp[16]{};
  uint64_t hostEpochSeconds = 0;
  int16_t hostUtcOffsetMinutes = 0;
  uint32_t hostTimeCapturedAt = 0;
  uint8_t helloVersion = 1;
  Mode sessionMode = Mode::Terminal;
  bool hasHostTime = false;
  knietty::FrameDecoder frameDecoder;
  size_t frameReadOffset = 0;
  uint8_t txBuffer[TX_BUFFER_SIZE]{};
  size_t txHead = 0;
  size_t txSize = 0;
  uint32_t nextTxSequence = 1;
  bool pairCommitPending = false;
  uint8_t pendingOutputBurstEnds = 0;

  void setState(State next);
  void startService();
  void stopService();
  void notifySessionEnd();
  void disconnectClient();
  void acceptIncoming();
  void pollTlsHandshake();
  void pollDiscovery();
  void pollHandshake();
  void pollFramedClient();
  void flushTx();
  void protocolError();
  bool enqueueTx(const uint8_t* data, size_t length);
  bool queueFrame(knietty::FrameType type, const uint8_t* payload, size_t length, uint32_t sequence);
  bool parseHello();
};
