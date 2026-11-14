#include "Ipv4.h"

#include <cctype>

namespace Ipv4 {

namespace {
// Reads one or more decimal digits into value, stopping at the first
// non-digit. Returns false when there are no digits or the number
// exceeds max.
bool parseUint(const char*& p, unsigned max, unsigned& value) {
  if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
  unsigned v = 0;
  while (std::isdigit(static_cast<unsigned char>(*p))) {
    v = v * 10 + static_cast<unsigned>(*p - '0');
    if (v > max) return false;
    ++p;
  }
  value = v;
  return true;
}
}  // namespace

bool parseCidr(const char* text, uint8_t ip[4], uint8_t& prefixLength) {
  if (!text) return false;

  const char* p = text;
  uint8_t octets[4];
  for (int i = 0; i < 4; i++) {
    unsigned octet;
    if (!parseUint(p, 255, octet)) return false;
    octets[i] = static_cast<uint8_t>(octet);
    if (i < 3) {
      if (*p != '.') return false;
      ++p;
    }
  }

  if (*p != '/') return false;
  ++p;

  unsigned prefix;
  if (!parseUint(p, 32, prefix)) return false;
  if (*p != '\0') return false;

  for (int i = 0; i < 4; i++) ip[i] = octets[i];
  prefixLength = static_cast<uint8_t>(prefix);
  return true;
}

bool isUsableHostIp(const uint8_t ip[4], uint8_t prefixLength) {
  if (!ip || prefixLength > 32) return false;
  if (ip[0] == 0 || ip[0] == 127 || ip[0] >= 224) return false;

  const uint32_t value = (static_cast<uint32_t>(ip[0]) << 24) | (static_cast<uint32_t>(ip[1]) << 16) |
                         (static_cast<uint32_t>(ip[2]) << 8) | ip[3];
  const uint32_t hostMask = prefixLength == 0 ? 0xFFFFFFFFu : (1u << (32 - prefixLength)) - 1;
  const uint32_t host = value & hostMask;
  return host != 0 && host != hostMask;
}

}  // namespace Ipv4
