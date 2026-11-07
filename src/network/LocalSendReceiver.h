#pragma once
#if defined(CROSSPOINT_LOCALSEND) && CROSSPOINT_LOCALSEND
#include <NetworkServer.h>

#include "LocalSendSession.h"

class LocalSendReceiver {
 public:
  using CancelCheck = bool (*)(void*);
  LocalSendReceiver();
  ~LocalSendReceiver();
  bool begin(CancelCheck check, void* context);
  void stop();
  void poll();
  void pollConsent();
  const localsend::Session* pendingOffer() const;
  void decide(bool accept);
  bool setDestination(const char* path);
  static bool validDeviceId(const char* id);
  static bool validFingerprint(const char* id);
  bool setAdvertisedId(const char* id);
  bool setFingerprint(const char* id);
  const char* deviceFingerprint() const { return fingerprint; }
  const char* destinationFolder() const { return destination; }
  uint32_t completedSequence() const { return savedSequence; }
  const char* completedName() const { return savedName; }
  uint32_t completedBytes() const { return savedBytes; }

 private:
  NetworkServer listener{53317, 5};
  localsend::Session session;
  NetworkClient pendingClient;
  uint32_t pendingSince = 0;
  CancelCheck cancelCheck = nullptr;
  void* cancelContext = nullptr;
  int discoverySocket = -1;
  uint32_t lastAnnouncement = 0, lastReply = 0;
  char fingerprint[33]{};
  char advertisedAlias[64] = "CrossPoint";
  char destination[96] = "/Books/LocalSend";
  char savedName[129]{};
  uint32_t savedBytes = 0;
  uint32_t savedSequence = 0;
  bool cancelled() const;
  bool receive(NetworkClient& client, uint8_t* bytes, size_t length, uint32_t started, uint32_t totalTimeout,
               uint32_t idleTimeout = 5000);
  int receiveLine(NetworkClient& client, char* line, size_t capacity, uint32_t started);
  void serve(NetworkClient& client);
  int upload(NetworkClient& client, const localsend::Request& request);
  void announce(bool initial);
  void discovery();
  void sendInfo(NetworkClient& client);
  static void reply(NetworkClient& client, int code, const char* body = "");
};
#endif
