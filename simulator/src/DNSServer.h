#pragma once
#include "WiFi.h"
enum DNSReplyCode {
  NoError = 0,
  FormError = 1,
  ServerFailure = 2,
  NonExistentDomain = 3,
  NotImplemented = 4,
  Refused = 5,
  YXDomain = 6,
  YXRRSet = 7,
  NXRRSet = 8,
  NotAuth = 9,
  NotZone = 10
};

class DNSServer {
public:
  void start(int port, const char *name, const IPAddress &ip) {}
  void start(int port, const char *name, const char *ip) {}
  void processNextRequest() {}
  void stop() {}
  void setErrorReplyCode(DNSReplyCode code) {}
};
