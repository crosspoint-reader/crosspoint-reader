#pragma once

#include <cstddef>
#include <cstdint>

#if __has_include("AgentcloudAuth.local.h")
#include "AgentcloudAuth.local.h"
#endif

#ifndef AGENTCLOUD_AUTH_BYTES
// AgentcloudAuth.local.h defines AGENTCLOUD_AUTH_BYTES as an eight-byte brace
// initializer. The all-zero fallback is deliberately invalid.
#define AGENTCLOUD_AUTH_BYTES {0, 0, 0, 0, 0, 0, 0, 0}
#endif

namespace agentcloud {

inline constexpr uint8_t AUTHENTICATOR[] = AGENTCLOUD_AUTH_BYTES;
static_assert(sizeof(AUTHENTICATOR) == 8, "Agentcloud authenticator must contain exactly eight bytes");

constexpr bool hasValidAuthenticator() {
  uint8_t combined = 0;
  for (const uint8_t byte : AUTHENTICATOR) combined |= byte;
  return combined != 0;
}

}  // namespace agentcloud
