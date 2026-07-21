#pragma once
#include <cstddef>
#include <cstdint>

#include "WiFi.h" // for IPAddress

class NetworkUDP {
public:
  NetworkUDP() {}
  uint8_t begin(uint16_t port) { return 1; }
  void stop() {}
  int beginPacket(const char *host, uint16_t port) { return 1; }
  int beginPacket(IPAddress ip, uint16_t port) { return 1; }
  size_t write(const uint8_t *buffer, size_t size) { return size; }
  int endPacket() { return 1; }
  int parsePacket() { return 0; }
  int read(unsigned char *buffer, size_t len) { return 0; }
  int read(char *buffer, size_t len) {
    return read((unsigned char *)buffer, len);
  }
  IPAddress remoteIP() { return IPAddress(); }
  uint16_t remotePort() { return 0; }
};
