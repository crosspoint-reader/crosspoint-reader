#pragma once
// RadioManager mock
struct RadioManagerMock {
  void ensureWifi() {}
  void ensureBle() {}
  void shutdown() {}
  bool isDisclaimerAcknowledged() { return true; }
  void setDisclaimerAcknowledged() {}
};
inline RadioManagerMock RADIO;
