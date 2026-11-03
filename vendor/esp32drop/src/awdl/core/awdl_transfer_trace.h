/* Diagnostic-only IPv6 classifier. No allocation, side effects or payload capture.
 * Direct TCP/UDP only; extension headers are deliberately not interpreted. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct AwdlTransferEvent {
  uint32_t ms, seq, ack;
  int32_t result;
  uint16_t sport, dport, size, zero_ttl;
  uint8_t peer[6], kind, flags;
  char direction;
};
static uint16_t awdl_trace_u16(const uint8_t* p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static uint32_t awdl_trace_u32(const uint8_t* p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
/* Skip an encoded DNS name, not its target. Every iteration consumes bytes;
 * pointers are never followed, including cyclic ones. */
static int awdl_trace_name_end(const uint8_t* p, int n, int at) {
  while (at < n) {
    unsigned k = p[at++];
    if (!k) return at;
    if ((k & 0xc0) == 0xc0) return at < n ? at + 1 : -1;
    if (k & 0xc0 || k > (unsigned)(n - at)) return -1;
    at += (int)k;
  }
  return -1;
}
static bool awdl_trace_classify(const uint8_t* ip, size_t n, struct AwdlTransferEvent* e) {
  if (n < 40 || ip[0] >> 4 != 6) return false;
  const unsigned plen = awdl_trace_u16(ip + 4);
  if (plen > n - 40) return false;
  const uint8_t* p = ip + 40;
  memset(e, 0, sizeof *e);
  if (ip[6] == 6) {
    if (plen < 20) return false;
    const unsigned h = (p[12] >> 4) * 4;
    if (h < 20 || h > plen || !(p[13] & 7)) return false;  // SYN/FIN/RST only
    e->kind = 6;
    e->flags = p[13];
    e->sport = awdl_trace_u16(p);
    e->dport = awdl_trace_u16(p + 2);
    e->seq = awdl_trace_u32(p + 4);
    e->ack = awdl_trace_u32(p + 8);
    e->size = (uint16_t)(plen - h);
    return true;
  }
  if (ip[6] != 17 || plen < 20) return false;
  if (awdl_trace_u16(p) != 5353 && awdl_trace_u16(p + 2) != 5353) return false;
  unsigned udp = awdl_trace_u16(p + 4);
  if (udp < 20 || udp > plen) return false;
  const uint8_t* dns = p + 8;
  const int dn = (int)udp - 8;
  if (!(dns[2] & 0x80)) return false;  // Responses only; never changes responder decisions
  int at = 12;
  unsigned qd = awdl_trace_u16(dns + 4);
  unsigned rr = awdl_trace_u16(dns + 6) + awdl_trace_u16(dns + 8) + awdl_trace_u16(dns + 10);
  for (unsigned i = 0; i < qd; ++i) {
    at = awdl_trace_name_end(dns, dn, at);
    if (at < 0 || at + 4 > dn) return false;
    at += 4;
  }
  for (unsigned i = 0; i < rr; ++i) {
    at = awdl_trace_name_end(dns, dn, at);
    if (at < 0 || at + 10 > dn) return false;
    unsigned rd = awdl_trace_u16(dns + at + 8);
    if (rd > (unsigned)(dn - at - 10)) return false;
    if (awdl_trace_u32(dns + at + 4) == 0) ++e->zero_ttl;
    at += 10 + (int)rd;
  }
  if (!e->zero_ttl) return false;
  e->kind = 17;
  e->size = (uint16_t)dn;
  return true;  // Generic zero-TTL RR, not a claim about which service was removed
}
