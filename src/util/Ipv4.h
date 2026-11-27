#pragma once

#include <cstdint>

namespace Ipv4 {

/**
 * Parse "a.b.c.d/prefix" into octets and prefix length.
 * The prefix is required and must be 0..32; each octet must be 0..255.
 * Returns false on any malformed input; outputs are only written on success.
 */
bool parseCidr(const char* text, uint8_t ip[4], uint8_t& prefixLength);

/**
 * True when ip is a usable host address inside prefixLength: first octet
 * 1..223 (not zero, loopback, multicast, or reserved) and host bits neither
 * all zero (network address) nor all ones (broadcast).
 */
bool isUsableHostIp(const uint8_t ip[4], uint8_t prefixLength);

}  // namespace Ipv4
