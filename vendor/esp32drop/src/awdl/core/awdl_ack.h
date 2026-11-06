#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Only plain cumulative ACKs are replaceable. Preserve SACK, ECN, zero windows,
// payload and extension headers rather than guessing at their semantics.
struct AwdlAck {
  uint8_t flow[36];  // IPv6 endpoints and TCP ports
  uint32_t seq, ack;
  bool valid;
};

static uint32_t awdl_ack_u32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static bool awdl_ack_parse(const uint8_t* ip, size_t len, struct AwdlAck* out) {
  out->valid = false;
  if (len < 60 || (ip[0] >> 4) != 6 || ip[6] != 6) return false;
  const size_t payload = ((size_t)ip[4] << 8) | ip[5];
  if (payload != len - 40) return false;
  const uint8_t* tcp = ip + 40;
  const size_t header = (tcp[12] >> 4) * 4u;
  if (header < 20 || header != payload || (tcp[12] & 15) || tcp[13] != 0x10) return false;
  if (!tcp[14] && !tcp[15]) return false;
  for (size_t i = 20; i < header;) {
    const uint8_t kind = tcp[i];
    if (!kind) break;
    if (kind == 1) {
      ++i;
      continue;
    }
    if (kind != 8 || header - i < 10 || tcp[i + 1] != 10) return false;
    i += 10;
  }
  memcpy(out->flow, ip + 8, 32);
  memcpy(out->flow + 32, tcp, 4);
  out->seq = awdl_ack_u32(tcp + 4);
  out->ack = awdl_ack_u32(tcp + 8);
  out->valid = true;
  return true;
}

static bool awdl_ack_supersedes(const struct AwdlAck* newer, const struct AwdlAck* older) {
  if (!newer->valid || !older->valid || newer->seq != older->seq ||
      memcmp(newer->flow, older->flow, sizeof(newer->flow)))
    return false;
  const uint32_t advance = newer->ack - older->ack;
  return advance != 0 && advance < 0x80000000u;
}
