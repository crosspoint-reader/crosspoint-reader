#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../vendor/esp32drop/src/awdl/core/awdl_ack.h"

static void put32(uint8_t* p, uint32_t n) {
  p[0] = n >> 24;
  p[1] = n >> 16;
  p[2] = n >> 8;
  p[3] = n;
}
static void packet(uint8_t* p, uint32_t ack) {
  memset(p, 0, 100);
  p[0] = 0x60;
  p[5] = 20;
  p[6] = 6;
  p[8] = 0xfe;
  p[24] = 0xfe;
  p[40] = 0x22;
  p[41] = 0x42;
  p[42] = 0x12;
  p[43] = 0x34;
  p[52] = 0x50;
  p[53] = 0x10;
  p[54] = 0x16;
  p[55] = 0x80;
  put32(p + 44, 100);
  put32(p + 48, ack);
}
int main(void) {
  uint8_t p[100];
  struct AwdlAck a, b;
  packet(p, 1000);
  assert(awdl_ack_parse(p, 60, &a));
  for (size_t n = 0; n < 60; ++n) assert(!awdl_ack_parse(p, n, &b));
  assert(!awdl_ack_parse(p, 61, &b));
  packet(p, 2000);
  assert(awdl_ack_parse(p, 60, &b));
  assert(awdl_ack_supersedes(&b, &a));
  assert(!awdl_ack_supersedes(&a, &b));
  assert(!awdl_ack_supersedes(&a, &a));
  for (int i = 0; i < 36; ++i) {
    b.flow[i] ^= 1;
    assert(!awdl_ack_supersedes(&b, &a));
    b.flow[i] ^= 1;
  }
  b.seq++;
  assert(!awdl_ack_supersedes(&b, &a));
  b.seq--;
  a.ack = 0xfffffff0u;
  b.ack = 16;
  assert(awdl_ack_supersedes(&b, &a));
  assert(!awdl_ack_supersedes(&a, &b));
  b.ack = a.ack + 0x80000000u;
  assert(!awdl_ack_supersedes(&b, &a));
  // Non-ACK controls, ECN and reserved bits must keep their original treatment.
  for (int flags = 0; flags < 256; ++flags) {
    packet(p, 2000);
    p[53] = flags;
    assert(awdl_ack_parse(p, 60, &b) == (flags == 0x10));
  }
  packet(p, 2000);
  p[54] = p[55] = 0;
  assert(!awdl_ack_parse(p, 60, &b));
  packet(p, 2000);
  p[52] |= 1;
  assert(!awdl_ack_parse(p, 60, &b));
  packet(p, 2000);
  p[6] = 0;
  assert(!awdl_ack_parse(p, 60, &b));
  packet(p, 2000);
  p[5] = 21;
  assert(!awdl_ack_parse(p, 61, &b));
  // Timestamp options are safe; SACK and malformed options are not filtered.
  packet(p, 2000);
  p[5] = 32;
  p[52] = 0x80;
  p[60] = p[61] = 1;
  p[62] = 8;
  p[63] = 10;
  assert(awdl_ack_parse(p, 72, &b));
  p[62] = 5;
  assert(!awdl_ack_parse(p, 72, &b));
  p[62] = 8;
  p[63] = 9;
  assert(!awdl_ack_parse(p, 72, &b));
  p[63] = 10;
  p[60] = 8;
  p[61] = 10;
  p[70] = 8;
  p[71] = 10;
  assert(!awdl_ack_parse(p, 72, &b));
  // Controlled queue workload: all older cumulative ACKs can be skipped,
  // but duplicate ACKs (used for fast retransmit) remain eligible.
  struct AwdlAck queue[16];
  for (int i = 0; i < 16; ++i) {
    packet(p, 1000 + i * 1200);
    assert(awdl_ack_parse(p, 60, &queue[i]));
  }
  int kept = 0;
  for (int i = 0; i < 16; ++i) kept += !awdl_ack_supersedes(&queue[15], &queue[i]);
  assert(kept == 1);
  for (int i = 0; i < 16; ++i) queue[i] = queue[15];
  kept = 0;
  for (int i = 0; i < 16; ++i) kept += !awdl_ack_supersedes(&queue[15], &queue[i]);
  assert(kept == 16);
  printf(
      "ACK policy: bounds, options, flags, flow isolation, wrap, duplicates passed; synthetic advancing queue 16 -> 1 "
      "eligible frames (not hardware speed)\n");
}
