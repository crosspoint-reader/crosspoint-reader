#pragma once

#include <Arduino.h>
#include <NetworkClient.h>

#include <cstddef>
#include <cstdint>

// Allocation-bounded TLS 1.3 server for knietty. The device identity and up to
// four pinned host identities live in one-record NVS blobs. TLS authenticates
// possession of each private key; the application pins the self-signed leaf
// fingerprints after physical approval.
class TerminalTls {
 public:
  enum class HandshakeResult : uint8_t { Pending, Connected, Failed };

  TerminalTls() = default;
  ~TerminalTls();

  bool begin(const char* hostname);
  void end();
  bool attach(NetworkClient&& incoming);
  HandshakeResult pollHandshake();
  void stop();

  int available();
  int read();
  int read(uint8_t* output, size_t length);
  size_t write(const uint8_t* data, size_t length);
  bool connected();

  bool peerIsPaired() const { return peerPaired; }
  bool peerNameChanged(const char* name) const;
  bool trustPeer(const char* name);
  bool canTrustPeer() const { return peerPaired || pairStore.count < MAX_PAIRED_HOSTS; }
  const char* pairedPeerName(uint8_t index) const;
  bool formatPairedPeerFingerprint(uint8_t index, char* output, size_t outputSize) const;
  bool forgetPeer(uint8_t index);
  bool forgetAllPeers();
  uint8_t pairedPeerCount() const { return pairStore.count; }
  const char* pairingCode() const { return sasCode; }
  const char* deviceFingerprintText() const { return deviceFingerprint; }
  uint32_t handshakeDurationMs() const { return handshakeMs; }
  uint32_t handshakeHeapFloor() const { return handshakeMinHeap; }

 private:
  static constexpr size_t CERTIFICATE_CAPACITY = 768;
  static constexpr size_t PRIVATE_KEY_CAPACITY = 256;
  static constexpr size_t FINGERPRINT_SIZE = 32;
  static constexpr size_t MAX_PAIRED_HOSTS = 4;
  static constexpr size_t HOST_NAME_SIZE = 33;

  struct IdentityBlob {
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t certificateLength = 0;
    uint16_t privateKeyLength = 0;
    uint16_t reserved = 0;
    uint8_t certificate[CERTIFICATE_CAPACITY]{};
    uint8_t privateKey[PRIVATE_KEY_CAPACITY]{};
    uint32_t crc = 0;
  };

  struct PairedHost {
    uint8_t fingerprint[FINGERPRINT_SIZE]{};
    char name[HOST_NAME_SIZE]{};
  };

  struct PairStore {
    uint32_t magic = 0;
    uint16_t version = 0;
    uint8_t count = 0;
    uint8_t reserved = 0;
    PairedHost hosts[MAX_PAIRED_HOSTS]{};
    uint32_t crc = 0;
  };

  IdentityBlob identity;
  PairStore pairStore;
  PairStore stagedPairStore;
  NetworkClient transport;
  void* context = nullptr;
  void* session = nullptr;
  uint8_t peerFingerprint[FINGERPRINT_SIZE]{};
  char sasCode[7]{};
  char deviceFingerprint[24]{};
  bool ready = false;
  bool peerReady = false;
  bool peerPaired = false;
  uint32_t handshakeStartedAt = 0;
  uint32_t handshakeMs = 0;
  uint32_t handshakeMinHeap = 0;

  bool loadOrCreateIdentity(const char* hostname);
  bool generateIdentity(const char* hostname);
  void loadPairStore();
  bool commitPairStore(PairStore& candidate);
  bool capturePeerIdentity();
  void updatePairingCode();
};
