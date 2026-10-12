#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

// "f0-9f-c2-31-b8-0c": how the network screens log a MAC or BSSID, and how the reader's own MAC is
// stored (WifiCredentialStore::setLastKnownMacAddress).
inline std::string formatMacDashed(const uint8_t mac[6]) {
  char text[18];
  snprintf(text, sizeof(text), "%02x-%02x-%02x-%02x-%02x-%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return std::string(text);
}
